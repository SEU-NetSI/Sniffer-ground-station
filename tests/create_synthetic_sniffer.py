#!/usr/bin/env python3
"""Create a deterministic, explicitly synthetic binary Sniffer capture."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from analyze_sniffer_binary import MAGIC, META


def main() -> None:
    destination = Path("testdata/synthetic_sniffer.bin")
    destination.parent.mkdir(parents=True, exist_ok=True)
    wrap = 1 << 40
    frames = []
    timestamps = (wrap - 2000, wrap - 1000, 500, 1600, 3000, 4300)
    sources = (5, 8, 5, 8, 5, 8)
    for index, (source, timestamp) in enumerate(zip(sources, timestamps)):
        payload = bytes((source, index, 0xA5, 0x5A))
        frames.append(META.pack(MAGIC, source, 100 + index, len(payload), timestamp) + payload)
    destination.write_bytes(b"".join(frames))
    print(destination)


if __name__ == "__main__":
    main()
