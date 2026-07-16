#!/usr/bin/env python3
"""Parse packed Sniffer meta+payload files and produce phase analysis outputs."""

from __future__ import annotations

import argparse
import csv
import statistics
import struct
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterable, Sequence

META = struct.Struct("<IHHHQ")
MAGIC = 0x0000BBBB
MAGIC_BYTES = struct.pack("<I", MAGIC)
MAX_MESSAGE_SIZE = 4096
DEFAULT_TICK_HZ = 499.2e6 * 128.0
DEFAULT_PERIOD_MS = 30.0


@dataclass(frozen=True, slots=True)
class PacketRecord:
    arrival_order: int
    input_offset: int
    magic: int
    rx_timestamp_ticks: int
    source_id: int
    sequence: int
    message_length: int
    payload: bytes = b""
    rx_time_cont_ticks: int | None = None
    rx_time_ms: float | None = None
    delta_ms: float | None = None


@dataclass(frozen=True, slots=True)
class ParseError:
    input_offset: int
    kind: str
    detail: str


@dataclass(frozen=True, slots=True)
class ParseResult:
    records: tuple[PacketRecord, ...]
    errors: tuple[ParseError, ...]
    input_size: int
    consumed_bytes: int
    remaining_bytes: int
    resynchronizations: int


def parse_binary(data: bytes, *, max_message_size: int = MAX_MESSAGE_SIZE) -> ParseResult:
    """Parse records, scanning for the next magic after corrupt bytes."""
    records: list[PacketRecord] = []
    errors: list[ParseError] = []
    offset = 0
    resynchronizations = 0
    seen: set[tuple[int, int, int, bytes]] = set()
    while offset < len(data):
        remaining = len(data) - offset
        if remaining < META.size:
            errors.append(ParseError(offset, "truncated_meta", f"remaining={remaining}, required={META.size}"))
            break

        if data[offset : offset + 4] != MAGIC_BYTES:
            next_offset = data.find(MAGIC_BYTES, offset + 1)
            skipped_to = len(data) if next_offset < 0 else next_offset
            errors.append(ParseError(offset, "magic_mismatch", f"discarded={skipped_to - offset}"))
            if next_offset < 0:
                break
            resynchronizations += 1
            offset = next_offset
            continue

        magic, source, sequence, message_length, rx_time = META.unpack_from(data, offset)
        if message_length > max_message_size:
            errors.append(ParseError(offset, "invalid_length", f"message_length={message_length}, max={max_message_size}"))
            next_offset = data.find(MAGIC_BYTES, offset + 1)
            if next_offset < 0:
                break
            resynchronizations += 1
            offset = next_offset
            continue

        end = offset + META.size + message_length
        if end > len(data):
            errors.append(ParseError(offset, "truncated_payload", f"declared={message_length}, available={len(data) - offset - META.size}"))
            next_offset = data.find(MAGIC_BYTES, offset + META.size)
            if next_offset < 0:
                break
            resynchronizations += 1
            offset = next_offset
            continue

        records.append(PacketRecord(
            arrival_order=len(records), input_offset=offset, magic=magic,
            rx_timestamp_ticks=rx_time, source_id=source, sequence=sequence,
            message_length=message_length, payload=data[offset + META.size : end],
        ))
        identity = (source, sequence, rx_time, data[offset + META.size : end])
        if identity in seen:
            errors.append(ParseError(offset, "duplicate_record", "same source, sequence, timestamp, and payload"))
        seen.add(identity)
        if rx_time >> 40:
            errors.append(ParseError(offset, "timestamp_high_bits", f"raw=0x{rx_time:016X}; low 40 bits retained"))
        offset = end

    return ParseResult(tuple(records), tuple(errors), len(data), offset,
                       len(data) - offset, resynchronizations)


def read_binary(path: Path, *, max_message_size: int = MAX_MESSAGE_SIZE) -> ParseResult:
    return parse_binary(path.read_bytes(), max_message_size=max_message_size)


def analyze_records(records: Iterable[PacketRecord], *, timestamp_bits: int = 40,
                    tick_hz: float = DEFAULT_TICK_HZ) -> tuple[PacketRecord, ...]:
    """Unwrap FIFO receiver timestamps, then use the first record as time zero."""
    if not 1 <= timestamp_bits <= 64:
        raise ValueError("timestamp_bits must be in [1, 64]")
    if tick_hz <= 0:
        raise ValueError("tick_hz must be positive")
    ordered = sorted(records, key=lambda record: record.arrival_order)
    mask = (1 << timestamp_bits) - 1
    wrap = 1 << timestamp_bits
    wrap_count = 0
    previous: int | None = None
    first_continuous: int | None = None
    analyzed: list[PacketRecord] = []
    for record in ordered:
        current = record.rx_timestamp_ticks & mask
        # Records are emitted FIFO from one DW3000 receiver clock. A long gap can
        # cross a 40-bit wrap while producing a decrease smaller than half a wrap.
        if previous is not None and current < previous:
            wrap_count += 1
        continuous = current + wrap_count * wrap
        if first_continuous is None:
            first_continuous = continuous
        analyzed.append(replace(record, rx_timestamp_ticks=current,
                                rx_time_cont_ticks=continuous,
                                rx_time_ms=(continuous - first_continuous) * 1000.0 / tick_hz))
        previous = current

    groups: dict[int, list[PacketRecord]] = {}
    for record in analyzed:
        groups.setdefault(record.source_id, []).append(record)
    delta_by_arrival: dict[int, float | None] = {}
    for group in groups.values():
        chronological = sorted(group, key=lambda record: record.rx_time_cont_ticks)
        previous_ms: float | None = None
        for record in chronological:
            assert record.rx_time_ms is not None
            delta_by_arrival[record.arrival_order] = (
                None if previous_ms is None else record.rx_time_ms - previous_ms
            )
            previous_ms = record.rx_time_ms
    return tuple(replace(record, delta_ms=delta_by_arrival[record.arrival_order])
                 for record in analyzed)


CSV_FIELDS = ("arrival_order", "input_offset", "magic", "source_id", "sequence",
              "message_length", "rx_timestamp_ticks", "rx_time_cont_ticks", "rx_time_ms", "delta_ms")


def _row(record: PacketRecord) -> dict[str, object]:
    return {
        "arrival_order": record.arrival_order, "input_offset": record.input_offset,
        "magic": f"0x{record.magic:08X}", "source_id": record.source_id,
        "sequence": record.sequence, "message_length": record.message_length,
        "rx_timestamp_ticks": record.rx_timestamp_ticks,
        "rx_time_cont_ticks": record.rx_time_cont_ticks,
        "rx_time_ms": "" if record.rx_time_ms is None else f"{record.rx_time_ms:.12f}",
        "delta_ms": "" if record.delta_ms is None else f"{record.delta_ms:.12f}",
    }


def write_records_csv(path: Path, records: Sequence[PacketRecord]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(_row(record) for record in records)


def sequence_stats(records: Sequence[PacketRecord]) -> tuple[int, int]:
    jumps = duplicates = 0
    for previous, current in zip(records, records[1:]):
        step = (current.sequence - previous.sequence) & 0xFFFF
        if step == 0:
            duplicates += 1
        elif step != 1:
            jumps += 1
    return jumps, duplicates


def write_outputs(output: Path, records: Sequence[PacketRecord], parsed: ParseResult,
                  *, input_path: Path, period_ms: float, timestamp_bits: int, tick_hz: float) -> None:
    output.mkdir(parents=True, exist_ok=True)
    write_records_csv(output / "parsed_records.csv", records)
    groups: dict[int, list[PacketRecord]] = {}
    for record in records:
        groups.setdefault(record.source_id, []).append(record)
    for source, group in sorted(groups.items()):
        chronological = sorted(group, key=lambda record: record.rx_time_cont_ticks)
        write_records_csv(output / f"group_source_{source}.csv", chronological)
    with (output / "parse_errors.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(("input_offset", "kind", "detail"))
        writer.writerows((error.input_offset, error.kind, error.detail) for error in parsed.errors)

    errors = parsed.errors
    lines = [f"input={input_path}", f"file_size={parsed.input_size}",
             f"records={len(records)}", f"consumed_bytes={parsed.consumed_bytes}",
             f"remaining_bytes={parsed.remaining_bytes}",
             f"sources={','.join(map(str, sorted(groups)))}", f"parse_diagnostics={len(errors)}",
             f"magic_errors={sum(e.kind == 'magic_mismatch' for e in errors)}",
             f"resynchronizations={parsed.resynchronizations}",
             f"truncated_meta={sum(e.kind == 'truncated_meta' for e in errors)}",
             f"truncated_payload={sum(e.kind == 'truncated_payload' for e in errors)}",
             f"invalid_lengths={sum(e.kind == 'invalid_length' for e in errors)}",
             f"duplicate_records={sum(e.kind == 'duplicate_record' for e in errors)}",
             f"timestamp_high_bits={sum(e.kind == 'timestamp_high_bits' for e in errors)}",
             f"period_ms={period_ms}", f"timestamp_bits={timestamp_bits}", f"tick_hz={tick_hz:.1f}"]
    for source, group in sorted(groups.items()):
        chronological = sorted(group, key=lambda record: record.rx_time_cont_ticks)
        deltas = [r.delta_ms for r in chronological if r.delta_ms is not None]
        jumps, duplicates = sequence_stats(group)
        first = chronological[0].rx_time_ms
        last = chronological[-1].rx_time_ms
        lines.extend((f"source_{source}.count={len(group)}", f"source_{source}.first_ms={first:.12f}",
                      f"source_{source}.last_ms={last:.12f}",
                      f"source_{source}.mean_delta_ms={statistics.fmean(deltas):.12f}" if deltas else f"source_{source}.mean_delta_ms=NA",
                      f"source_{source}.std_delta_ms={statistics.pstdev(deltas):.12f}" if deltas else f"source_{source}.std_delta_ms=NA",
                      f"source_{source}.min_delta_ms={min(deltas):.12f}" if deltas else f"source_{source}.min_delta_ms=NA",
                      f"source_{source}.max_delta_ms={max(deltas):.12f}" if deltas else f"source_{source}.max_delta_ms=NA",
                      f"source_{source}.sequence_jumps={jumps}", f"source_{source}.duplicate_sequences={duplicates}"))
    (output / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")


def phase_coordinates(records: Sequence[PacketRecord], period_ms: float) -> tuple[list[float], list[float]]:
    if period_ms <= 0:
        raise ValueError("period_ms must be positive")
    times = [record.rx_time_ms for record in records]
    if any(value is None for value in times):
        raise ValueError("records must be analyzed before plotting")
    numeric = [float(value) for value in times]
    return [value % period_ms for value in numeric], [value / period_ms for value in numeric]


def plot_phase(records: Sequence[PacketRecord], output_path: Path, period_ms: float) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as error:
        raise RuntimeError("matplotlib is required for phase.png; install requirements.txt") from error
    groups: dict[int, list[PacketRecord]] = {}
    for record in records:
        groups.setdefault(record.source_id, []).append(record)
    fig, ax = plt.subplots(figsize=(8, 8))
    for index, (source, group) in enumerate(sorted(groups.items())):
        x, y = phase_coordinates(group, period_ms)
        ax.plot(x, y, ".", label=f"rng_src={source}", color=plt.cm.tab10(index % 10), markersize=1)
    ax.set_xlim(0, period_ms)
    ax.set_xlabel("time % period (ms)")
    ax.set_ylabel(f"time / period   (period = {period_ms:.6f} ms)")
    ax.set_title("Phase diagram: arrival time modulo common period")
    ax.grid(True, alpha=0.3)
    if groups:
        ax.legend()
    fig.tight_layout()
    fig.savefig(output_path, dpi=120)
    plt.close(fig)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--period-ms", type=float, default=DEFAULT_PERIOD_MS)
    parser.add_argument("--timestamp-bits", type=int, default=40)
    parser.add_argument("--tick-hz", type=float, default=DEFAULT_TICK_HZ)
    parser.add_argument("--max-message-size", type=int, default=MAX_MESSAGE_SIZE)
    args = parser.parse_args(argv)
    output = args.output or Path("outputs_binary") / args.input.stem
    parsed = read_binary(args.input, max_message_size=args.max_message_size)
    analyzed = analyze_records(parsed.records, timestamp_bits=args.timestamp_bits, tick_hz=args.tick_hz)
    write_outputs(output, analyzed, parsed, input_path=args.input, period_ms=args.period_ms,
                  timestamp_bits=args.timestamp_bits, tick_hz=args.tick_hz)
    plot_phase(analyzed, output / "phase.png", args.period_ms)
    print(f"Parsed {len(analyzed)} record(s), {len(parsed.errors)} diagnostic(s); output: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
