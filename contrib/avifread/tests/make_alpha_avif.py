#!/usr/bin/env python3
# make_alpha_avif.py - build a two-item (colour + alpha) AVIF still image
# from two single-item AVIF files produced by ffmpeg.
#
# ffmpeg 7.0's AVIF muxer cannot write the alpha auxiliary item, so this
# script remuxes: it extracts the AV1 item bitstreams and the av1C/ispe/
# pixi/colr properties from both inputs and writes a standards-shaped
# AVIF with an iref/auxl reference and an auxC alpha URN.
#
# usage: make_alpha_avif.py colour.avif alpha.avif out.avif

import struct
import sys


def boxes(data, start=0, end=None):
    """Iterate (type, payload_offset, payload_size, box_end)."""
    if end is None:
        end = len(data)
    off = start
    while off + 8 <= end:
        (size,) = struct.unpack_from(">I", data, off)
        typ = data[off + 4:off + 8]
        hdr = 8
        if size == 1:
            (size,) = struct.unpack_from(">Q", data, off + 8)
            hdr = 16
        elif size == 0:
            size = end - off
        yield typ, off + hdr, size - hdr, off + size
        off += size


def find_box(data, want, start=0, end=None):
    for typ, off, size, _ in boxes(data, start, end):
        if typ == want:
            return off, size
    return None


def parse_avif(path):
    data = open(path, "rb").read()
    meta_off, meta_size = find_box(data, b"meta")
    # meta is a full box: skip version+flags
    meta = (meta_off + 4, meta_off + meta_size)

    iloc_off, iloc_size = find_box(data, b"iloc", *meta)
    iinf_off, iinf_size = find_box(data, b"iinf", *meta)
    iprp_off, iprp_size = find_box(data, b"iprp", *meta)

    # item 1 extents from iloc (version 0, as written by ffmpeg)
    p = iloc_off + 4  # version+flags
    sizes = data[p]
    off_sz, len_sz = sizes >> 4, sizes & 15
    p += 2  # sizes byte + base_offset/index byte
    (count,) = struct.unpack_from(">H", data, p)
    p += 2
    assert count == 1, "expected a single-item AVIF"
    p += 2  # item_id
    p += 2  # data_reference_index
    (n_ext,) = struct.unpack_from(">H", data, p)
    p += 2
    assert n_ext == 1
    ext_off = int.from_bytes(data[p:p + off_sz], "big")
    p += off_sz
    ext_len = int.from_bytes(data[p:p + len_sz], "big")
    bitstream = data[ext_off:ext_off + ext_len]

    # properties from ipco
    ipco_off, ipco_size = find_box(data, b"ipco", iprp_off, iprp_off + iprp_size)
    props = {}
    for typ, off, size, _ in boxes(data, ipco_off, ipco_off + ipco_size):
        props[typ.decode("latin1")] = data[off - 8:off + size]  # whole box

    return {
        "bitstream": bitstream,
        "ispe": props["ispe"],
        "pixi": props["pixi"],
        "av1c": props["av1C"],
        "colr": props.get("colr"),
    }


def box(typ, payload):
    return struct.pack(">I", 8 + len(payload)) + typ + payload


def full_box(typ, version, flags, payload):
    return box(typ, bytes([version]) + flags.to_bytes(3, "big") + payload)


def build(color, alpha):
    ftyp = box(b"ftyp", b"avif" + (0).to_bytes(4, "big") +
               b"avifmif1miafMA1B")

    hdlr = full_box(b"hdlr", 0, 0,
                    (0).to_bytes(4, "big") + b"pict" + b"\0" * 12 + b"\0")
    pitm = full_box(b"pitm", 0, 0, (1).to_bytes(2, "big"))

    # iloc v0, 4-byte offsets and lengths, one extent per item
    iloc_body = bytes([0x44, 0x00]) + (2).to_bytes(2, "big")
    offsets_pos = []
    for item_id in (1, 2):
        iloc_body += item_id.to_bytes(2, "big")
        iloc_body += (0).to_bytes(2, "big")      # data_reference_index
        iloc_body += (1).to_bytes(2, "big")      # extent_count
        offsets_pos.append(len(iloc_body))
        iloc_body += b"\0" * 4                    # extent_offset (patched)
        iloc_body += len(color["bitstream"] if item_id == 1
                         else alpha["bitstream"]).to_bytes(4, "big")
    iloc = full_box(b"iloc", 0, 0, iloc_body)

    def infe(item_id, name):
        return full_box(b"infe", 2, 0,
                        item_id.to_bytes(2, "big") + (0).to_bytes(2, "big") +
                        b"av01" + name + b"\0")

    iinf = full_box(b"iinf", 0, 0,
                    (2).to_bytes(2, "big") + infe(1, b"Color") +
                    infe(2, b"Alpha"))

    alpha_urn = b"urn:mpeg:mpegB:cicp:systems:auxiliary:alpha"
    # alpha pixi: 1 channel, bit depth taken from the alpha source file
    alpha_bits = alpha["pixi"][8 + 4 + 1]  # box hdr + ver/flags + nch
    alpha_pixi = full_box(b"pixi", 0, 0, bytes([1, alpha_bits]))
    ipco_props = [color["ispe"], color["pixi"], color["av1c"]]
    if color["colr"]:
        ipco_props.append(color["colr"])
    n_color_props = len(ipco_props)
    ipco_props += [alpha["ispe"], alpha_pixi, alpha["av1c"],
                   full_box(b"auxC", 0, 0, alpha_urn + b"\0")]
    ipco = box(b"ipco", b"".join(ipco_props))

    def ipma_entry(item_id, indices, essential):
        e = item_id.to_bytes(2, "big") + bytes([len(indices)])
        for i in indices:
            e += bytes([(0x80 if i in essential else 0) | i])
        return e

    ipma = full_box(b"ipma", 0, 0, (2).to_bytes(4, "big") +
                    ipma_entry(1, range(1, n_color_props + 1), {1, 3}) +
                    ipma_entry(2, range(n_color_props + 1,
                                        n_color_props + 5),
                               {n_color_props + 1, n_color_props + 3}))
    iprp = box(b"iprp", ipco + ipma)

    auxl = box(b"auxl", (2).to_bytes(2, "big") + (1).to_bytes(2, "big") +
               (1).to_bytes(2, "big"))
    iref = full_box(b"iref", 0, 0, auxl)

    meta = full_box(b"meta", 0, 0,
                    hdlr + pitm + iloc + iinf + iprp + iref)

    mdat_header = struct.pack(">I", 8 + len(color["bitstream"]) +
                              len(alpha["bitstream"])) + b"mdat"
    base = len(ftyp) + len(meta) + len(mdat_header)

    # patch extent offsets
    iloc_body = bytearray(iloc[12:])
    for pos, off in zip(offsets_pos,
                        (base, base + len(color["bitstream"]))):
        iloc_body[pos:pos + 4] = off.to_bytes(4, "big")
    iloc = iloc[:12] + bytes(iloc_body)
    meta = full_box(b"meta", 0, 0,
                    hdlr + pitm + iloc + iinf + iprp + iref)
    assert len(meta) + len(ftyp) + len(mdat_header) == base

    return ftyp + meta + mdat_header + color["bitstream"] + alpha["bitstream"]


def main():
    color = parse_avif(sys.argv[1])
    alpha = parse_avif(sys.argv[2])
    out = build(color, alpha)
    open(sys.argv[3], "wb").write(out)
    print("wrote %s (%d bytes)" % (sys.argv[3], len(out)))


if __name__ == "__main__":
    main()
