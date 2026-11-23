# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for inspect_romfs.py on hand-built files.  python -I tools/gamedata/test_inspect_romfs.py"""
import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import inspect_romfs as ir  # noqa: E402


def mp_str(s):
    b = s.encode("utf-8")
    return bytes([0xA0 | len(b)]) + b


def msbt(labels_texts):
    """A little-endian UTF-16 MSBT with LBL1 (one hash slot) and TXT2."""
    lbl = bytearray(struct.pack("<I", 1))
    entries = bytearray()
    for i, (lab, _) in enumerate(labels_texts):
        n = lab.encode()
        entries += bytes([len(n)]) + n + struct.pack("<I", i)
    lbl += struct.pack("<II", len(labels_texts), 4 + 8) + entries
    txt = bytearray(struct.pack("<I", len(labels_texts)))
    body = bytearray()
    offs = []
    for _, t in labels_texts:
        offs.append(4 + 4 * len(labels_texts) + len(body))
        body += t
    txt += b"".join(struct.pack("<I", o) for o in offs) + body

    def sec(magic, b):
        s = magic + struct.pack("<I", len(b)) + b"\0" * 8 + bytes(b)
        return s + b"\xab" * (-len(s) % 16)

    secs = sec(b"LBL1", lbl) + sec(b"TXT2", txt)
    hdr = b"MsgStdBn" + b"\xff\xfe" + b"\0\0" + bytes([1, 3]) + struct.pack("<H", 2) + b"\0\0"
    hdr += struct.pack("<I", 0x20 + len(secs)) + b"\0" * 10
    return hdr + secs


class MsgpackTest(unittest.TestCase):
    def test_map_of_units(self):
        # {"units": [{"id": "PID_A", "lv": 3, "hp": 300, "x": -2, "f": 1.5, "on": true, "bin": b"\x01"}]}
        unit = (b"\x87" + mp_str("id") + mp_str("PID_A") + mp_str("lv") + b"\x03" + mp_str("hp") + b"\xcd\x01\x2c" + mp_str("x") + b"\xfe"
                + mp_str("f") + b"\xcb" + struct.pack(">d", 1.5) + mp_str("on") + b"\xc3" + mp_str("bin") + b"\xc4\x01\x01")
        data = b"\x81" + mp_str("units") + b"\x91" + unit
        v = ir.decode_msgpack(data)
        self.assertEqual(v, [{"units": [{"id": "PID_A", "lv": 3, "hp": 300, "x": -2, "f": 1.5, "on": True, "bin": {"$bin": "01"}}]}])

    def test_several_top_level_values_and_int_keys(self):
        data = b"\x91\x01" + b"\x81\x05" + mp_str("five")
        self.assertEqual(ir.decode_msgpack(data), [[1], {"5": "five"}])

    def test_plain_text_is_not_msgpack(self):
        self.assertIsNone(ir.decode_msgpack(b"hello world"))
        self.assertEqual(ir.kind(b"hello world\n"), "text")

    def test_long_forms_and_ext(self):
        s = "x" * 40
        data = b"\xdc\x00\x03" + b"\xd9\x28" + s.encode() + b"\xd2\xff\xff\xff\xff" + b"\xd4\x07\x09"
        self.assertEqual(ir.decode_msgpack(data), [[s, -1, {"$ext": 7, "hex": "09"}]])

    def test_not_msgpack(self):
        self.assertIsNone(ir.decode_msgpack(b"\xc1"))
        self.assertIsNone(ir.decode_msgpack(b"\x92\x01"))   # truncated array
        self.assertIsNone(ir.decode_msgpack(b""))


class MsbtTest(unittest.TestCase):
    def test_labels_and_tags(self):
        hello = "Hi ".encode("utf-16-le") + struct.pack("<HHHH", 0x0E, 1, 2, 2) + b"\x34\x12" + "you".encode("utf-16-le") + b"\0\0"
        data = msbt([("MPID_Anatolia", "Anatolia".encode("utf-16-le") + b"\0\0"), ("MID_Hi", hello)])
        out = ir.decode_msbt(data)
        self.assertEqual(out["messages"], {"MPID_Anatolia": "Anatolia", "MID_Hi": "Hi [1.2:3412]you"})
        self.assertEqual(ir.kind(data), "msbt")


class SarcTest(unittest.TestCase):
    def test_names(self):
        files = [("a/one.bin", b"\x81" + mp_str("k") + b"\x01"), ("two.txt", b"hello")]
        names = bytearray()
        offs = []
        for n, _ in files:
            offs.append(len(names) // 4)
            names += n.encode() + b"\0"
            names += b"\0" * (-len(names) % 4)
        blob = bytearray()
        nodes = bytearray()
        for (n, b), o in zip(files, offs):
            nodes += struct.pack("<IIII", 0, 0x01000000 | o, len(blob), len(blob) + len(b))
            blob += b
            blob += b"\0" * (-len(blob) % 8)
        sfat = b"SFAT" + struct.pack("<HHI", 0xC, len(files), 0x65) + nodes
        sfnt = b"SFNT" + struct.pack("<HH", 8, 0) + names
        data_off = 0x14 + len(sfat) + len(sfnt)
        hdr = b"SARC" + struct.pack("<HH", 0x14, 0xFEFF) + struct.pack("<II", data_off + len(blob), data_off) + struct.pack("<HH", 0x100, 0)
        data = hdr + sfat + sfnt + blob
        out = ir.decode(data)
        self.assertEqual(out["$sarc"], [{"name": "a/one.bin", "size": 4, "kind": "msgpack"}, {"name": "two.txt", "size": 5,
                                                                                               "kind": "text"}])
        with tempfile.TemporaryDirectory() as d:
            ir.decode(data, extract=d)
            with open(os.path.join(d, "a", "one.bin"), "rb") as f:
                self.assertEqual(f.read(), files[0][1])


class FindTest(unittest.TestCase):
    def test_find_utf8_and_utf16(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "Data"))
            with open(os.path.join(d, "Data", "person.bin"), "wb") as f:
                f.write(b"\x81" + mp_str("name") + mp_str("Troy"))
            with open(os.path.join(d, "Data", "msg.msbt"), "wb") as f:
                f.write(msbt([("MPID_Troy", "Troy".encode("utf-16-le") + b"\0\0")]))
            with open(os.path.join(d, "other.bin"), "wb") as f:
                f.write(b"nothing")
            import io
            from contextlib import redirect_stdout, redirect_stderr
            out = io.StringIO()
            with redirect_stdout(out), redirect_stderr(io.StringIO()):
                ir.main(["find", d, "Troy"])
            lines = sorted(out.getvalue().splitlines())
            self.assertEqual(lines, ["Data/msg.msbt\tTroy x1 (utf-8), Troy x1 (utf-16)", "Data/person.bin\tTroy x1 (utf-8)"])


if __name__ == "__main__":
    unittest.main()
