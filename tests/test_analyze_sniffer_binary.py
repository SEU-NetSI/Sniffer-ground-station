import struct
import unittest
from dataclasses import replace
from pathlib import Path

import analyze_sniffer_binary as subject


def frame(source=5, sequence=9, length=3, timestamp=123456, payload=b"abc"):
    return subject.META.pack(subject.MAGIC, source, sequence, length, timestamp) + payload


class BinarySnifferTests(unittest.TestCase):
    def test_layout_and_single_record(self):
        self.assertEqual(subject.META.size, 18)
        raw = frame()
        result = subject.parse_binary(raw)
        self.assertFalse(result.errors)
        record = result.records[0]
        self.assertEqual((record.input_offset, record.magic, record.source_id, record.sequence,
                          record.message_length, record.rx_timestamp_ticks, record.payload),
                         (0, subject.MAGIC, 5, 9, 3, 123456, b"abc"))
        self.assertEqual(struct.unpack_from("<Q", raw, 10)[0], 123456)

    def test_interleaved_sources_are_globally_unwrapped(self):
        wrap = 1 << 40
        data = b"".join((frame(8, 1, 0, wrap - 100, b""), frame(5, 7, 0, wrap - 20, b""),
                         frame(8, 2, 0, 30, b""), frame(5, 8, 0, 100, b"")))
        analyzed = subject.analyze_records(subject.parse_binary(data).records)
        self.assertEqual([r.rx_time_cont_ticks for r in analyzed], [wrap - 100, wrap - 20, wrap + 30, wrap + 100])
        self.assertEqual([r.source_id for r in analyzed], [8, 5, 8, 5])
        self.assertEqual(analyzed[0].rx_time_ms, 0.0)
        self.assertAlmostEqual(analyzed[-1].rx_time_ms,
                               200 * 1000.0 / subject.DEFAULT_TICK_HZ)

    def test_truncation_and_resynchronization(self):
        truncated = subject.parse_binary(frame()[:-1])
        self.assertEqual(truncated.errors[0].kind, "truncated_payload")
        valid = frame(source=8)
        recovered = subject.parse_binary(b"damage" + valid)
        self.assertEqual(len(recovered.records), 1)
        self.assertEqual(recovered.records[0].input_offset, 6)
        self.assertEqual(recovered.errors[0].kind, "magic_mismatch")
        self.assertEqual((recovered.consumed_bytes, recovered.remaining_bytes,
                          recovered.resynchronizations), (len(b"damage" + valid), 0, 1))

    def test_invalid_length_resynchronizes(self):
        bad = subject.META.pack(subject.MAGIC, 1, 1, 5000, 1)
        parsed = subject.parse_binary(bad + frame(source=2))
        self.assertEqual(parsed.errors[0].kind, "invalid_length")
        self.assertEqual(parsed.records[0].source_id, 2)

    def test_phase_coordinates(self):
        records = [replace(subject.PacketRecord(0, 0, subject.MAGIC, 0, 1, 1, 0), rx_time_ms=61.5)]
        x, y = subject.phase_coordinates(records, 60.0)
        self.assertAlmostEqual(x[0], 1.5)
        self.assertAlmostEqual(y[0], 1.025)

    def test_empty_file(self):
        parsed = subject.parse_binary(b"")
        self.assertEqual(parsed.records, ())
        self.assertEqual(parsed.errors, ())
        self.assertEqual((parsed.input_size, parsed.consumed_bytes, parsed.remaining_bytes,
                          parsed.resynchronizations), (0, 0, 0, 0))

    def test_high_timestamp_bits_and_duplicate_are_reported(self):
        raw = frame(timestamp=(0xABCDEF << 40) | 123)
        parsed = subject.parse_binary(raw + raw)
        self.assertEqual(len(parsed.records), 2)
        self.assertEqual([error.kind for error in parsed.errors],
                         ["timestamp_high_bits", "duplicate_record", "timestamp_high_bits"])
        analyzed = subject.analyze_records(parsed.records)
        self.assertEqual([record.rx_timestamp_ticks for record in analyzed], [123, 123])

    def test_group_delta_uses_chronological_order(self):
        records = (
            subject.PacketRecord(0, 0, subject.MAGIC, 300, 5, 1, 0),
            subject.PacketRecord(1, 18, subject.MAGIC, 100, 5, 2, 0),
            subject.PacketRecord(2, 36, subject.MAGIC, 200, 5, 3, 0),
        )
        analyzed = subject.analyze_records(records, timestamp_bits=40, tick_hz=1000.0)
        self.assertEqual([record.delta_ms for record in analyzed], [100.0, None, 100.0])


if __name__ == "__main__":
    unittest.main()
