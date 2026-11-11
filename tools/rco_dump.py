#!/usr/bin/env python3
"""Dumps a PS Vita RCO (CXML) resource file's element tree as XML-ish text.

usage: rco_dump.py file.rco [> file.xml]
Layout from the CXML format (header of table offsets, then a tree of elements
with typed attributes). File data is not extracted; only names, ids and values.
"""
import struct, sys

def main(path):
    d = open(path, "rb").read()
    assert d[:4] == b"RCOF", "not an RCO"
    (tree_off, tree_sz, idstr_off, idstr_sz, idint_off, idint_sz, str_off, str_sz, chr_off, chr_sz,
     sty_off, sty_sz, ia_off, ia_sz, fa_off, fa_sz, file_off, file_sz) = struct.unpack_from("<18I", d, 8)

    def cstr(base):
        end = d.index(b"\0", base)
        return d[base:end].decode("utf-8", "replace")

    def attr_value(t, raw):
        a, b = struct.unpack_from("<II", raw)
        if t == 1: return str(a)
        if t == 2: return repr(struct.unpack("<f", raw[:4])[0])
        if t == 3: return '"%s"' % d[str_off + a:str_off + a + b].decode("utf-8", "replace")
        if t == 4: return '"%s"' % d[chr_off + a * 2:chr_off + a * 2 + b].decode("utf-16-le", "replace")
        if t == 5: return "style:%08X" % struct.unpack_from("<I", d, sty_off + a * 4)[0]
        if t == 6: return "[%s]" % ",".join(str(x) for x in struct.unpack_from("<%dI" % b, d, ia_off + a * 4))
        if t == 7: return "[%s]" % ",".join("%g" % x for x in struct.unpack_from("<%df" % b, d, fa_off + a * 4))
        if t == 8: return "file@%X+%X" % (a, b)
        if t in (9, 10): return "id:%s" % cstr(idstr_off + a + 4)
        if t in (11, 12): return "idint:%08X" % struct.unpack_from("<I", d, idint_off + a + 4)[0]
        return "?%d:%08X,%08X" % (t, a, b)

    def element(off, depth):
        name_off, nattr, parent, prev, nxt, first, last = struct.unpack_from("<7I", d, tree_off + off)
        name = cstr(str_off + name_off)
        attrs = []
        for i in range(nattr):
            base = tree_off + off + 28 + i * 16
            an_off, t = struct.unpack_from("<II", d, base)
            attrs.append("%s=%s" % (cstr(str_off + an_off), attr_value(t, d[base + 8:base + 16])))
        print("  " * depth + "<" + name + (" " + " ".join(attrs) if attrs else "") + ">")
        child = first
        while child != 0xFFFFFFFF:
            child = element(child, depth + 1)
        return nxt

    element(0, 0)

if __name__ == "__main__":
    main(sys.argv[1])
