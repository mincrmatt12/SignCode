#!/usr/bin/env python3
import os
import binascii
import sys
import slotlib
import textwrap
import struct
import time
# dissects capture logs and outputs a list of packets
datastream = []
prefixes = lambda y: [y[:x] for x in range(1, len(y)+1)]
realtime = False

ignored_dataids = []
ignored_datastr = os.getenv("CAPLOG_INGORE_IDS")
if ignored_datastr:
    ignored_dataids = [int(x, 16) for x in ignored_datastr.split()]

if len(sys.argv) < 3:
    print("usage: {} [logicexport,simlog,realtime] <in>")
    exit(1)

if sys.argv[1] in prefixes("logicexport"):
    with open(sys.argv[2]) as f:
        f.readline()
        flag = False

        for i in f.readlines():
            i = i.rstrip('\n')
            row = i.split(',')

            v = int(row[-1][2:], base=16)
            if not flag and v not in (0xa6, 0xa5):
                continue
            flag = True
            datastream.append(v)
elif sys.argv[1] in prefixes("simlog"):
    with open(sys.argv[2], 'rb') as f:
        datastream = [x for x in f.read()]
elif sys.argv[1] in prefixes("realtime"):
    realtime = True
else:
    print("usage: {} [logicexport,simlog,realtime] <in>")
    exit(1)

ptr = 0
if not realtime:
    def read_buf(x):
        global ptr
        ptr += x
        if ptr > len(datastream):
            exit()
        return bytes(datastream[ptr-x:ptr])

    def read():
        header = bytearray(read_buf(3))
        while header[0] not in (0xa5, 0xa6):
            header[0:1] = header[1:2]
            header[2] = read_buf(1)[0]
        if header[1]:
            yield bytes(header) + read_buf(header[1])
        else:
            yield bytes(header)
else:
    files = [open(x, 'rb') for x in sys.argv[2:]]
    import select

    def read_exact(f, x):
        buf = bytearray()
        while len(buf) < x:
            buf += os.read(f.fileno(), x-len(buf))
        return buf
    
    def read():
        (ok, _, _) = select.select(files, [], [])
        for f in ok:
            header = read_exact(f, 3)
            while header[0] not in (0xa5, 0xa6):
                header[0:1] = header[1:2]
                header[2] = read_exact(f, 1)[0]
            yield bytes(header + read_exact(f, header[1]))

pnames = {
    0x10: "HANDSHAKE_INIT",
    0x11: "HANDSHAKE_RESP",
    0x12: "HANDSHAKE_OK",
    0x13: "HANDSHAKE_UOK",
    0x20: "DATA_TEMP",
    0x21: "DATA_UPDATE",
    0x22: "DATA_MOVE",
    0x23: "DATA_DEL",
    0x24: "DATA_FORGOT",
    0x30: "ACK_DATA_TEMP",
    0x31: "ACK_DATA_UPDATE",
    0x32: "ACK_DATA_MOVE",
    0x33: "ACK_DATA_DEL",
    0x40: "QUERY_FREE_HEAP",
    0x41: "QUERY_TIME",
    0x50: "RESET",
    0x51: "PING",
    0x52: "PONG",
    0x60: "UPDATE_CMD",
    0x61: "UPDATE_IMG_DATA",
    0x62: "UPDATE_IMG_START",
    0x63: "UPDATE_STATUS",
    0x70: "CONSOLE_MSG"
}

header_width = 5 + 2 + 2 + 2 + 2 + 20 + 2

tempcodes = {
    0b11: "Hot",
    0b10: "Warm",
    0b01: "Cold"
}

slot_temps = {}
slot_databufs = {}

def data_temp(dat, from_esp):
    slotid, tempcode = struct.unpack("<HB", bytes(dat))
    
    print(f": request to set {slotid:03x} ({slotlib.slot_types[slotid][0]}) to {tempcodes[tempcode]}")

def ack_data_temp(dat, from_esp):
    slotid, tempcode = struct.unpack("<HB", bytes(dat))
    slot_temps[slotid] = tempcode
    
    print(f": ack of request to set {slotid:03x} ({slotlib.slot_types[slotid][0]}) to {tempcodes[tempcode]}")

def data_update(dat, from_esp, is_move):
    # always from esp
    sid_frame, offs, totallen, totalupd = struct.unpack("<HHHH", dat[:8])

    start = bool(sid_frame & (1 << 15))
    end = bool(sid_frame & (1 << 14))

    slotid = sid_frame & 0xfff
    if slotid not in slot_databufs:
        slot_databufs[slotid] = bytearray(totallen)
    else:
        if len(slot_databufs[slotid]) > totallen:
            slot_databufs[slotid] = slot_databufs[slotid][:totallen]
        else:
            slot_databufs[slotid] += bytearray(totallen - len(slot_databufs[slotid]))

    endstart = {
            (True, False): "start",
            (False, True): "end",
            (True, True): "whole",
            (False, False): "middle"
    }[start, end]

    if from_esp:
        # patch in
        slot_databufs[slotid][offs:offs+len(dat)-8] = dat[8:]

    print(f": data {'move' if is_move else 'update'} [{endstart}] for {slotid:03x} ({slotlib.slot_types[slotid][0]}) @ {offs:04x}")
    if start:
        print(" "*header_width + f": total slot length {totallen}; total update length {totalupd}")
    if end and from_esp:
        if slotid in ignored_dataids:
            print(textwrap.indent("(data supressed due to environment var)", ' '*(header_width + 3)))
            return
        st = slotlib.slot_types[slotid][1]
        if totallen >= 512:
            print(textwrap.indent(st.get_formatted(st.parse(slot_databufs[slotid]), (offs + len(dat) - 8 - totalupd), (offs + len(dat) - 8)), ' ' * (header_width + 2) + '└'))
        else:
            print(textwrap.indent(st.get_formatted(st.parse(slot_databufs[slotid])), ' ' * (header_width + 2) + '└'))

updrescode = {
    0: "Ok",
    1: "NotEnoughSpace",
    2: "IllegalInternalState",
    3: "NAK"
}

def ack_data_update(dat, from_esp, is_move):
    slotid, upds, updl, code = struct.unpack("<HHHB", dat)

    print(f": ack of data {'move' if is_move else 'update'} for {slotid:03x} ({slotlib.slot_types[slotid][0]}) @ {upds:04x} of length {updl} with code {updrescode[code]}")

def data_del(dat, from_esp):
    slotid = struct.unpack("<H", dat)[0]

    if slotid in slot_databufs:
        slot_databufs[slotid] = bytearray()
    
    print(f": data delete for {slotid:03x} ({slotlib.slot_types[slotid][0]})")

def ack_data_del(dat, from_esp):
    slotid = struct.unpack("<H", dat)[0]
    
    print(f": ack of data delete for {slotid:03x} ({slotlib.slot_types[slotid][0]})")

timestatus = {
    0: "Ok",
    1: "NotSet"
}

def query_time(dat, from_esp):
    if not from_esp:
        print(f": request")
    else:
        code, t = struct.unpack("<BQ", dat)

        if code == 0:
            print(f": response of time {t} ({time.asctime(time.gmtime(t // 1000))} +{t % 1000}ms)")
        else:
            print(f": response with error code {timestatus[code]}")

def data_forgot(dat, from_esp):
    slotid, offs, size = struct.unpack("<HHH", dat)
    print(f": notification of data deletion for {slotid:03x} ({slotlib.slot_types[slotid][0]}) at {offs:04x} of length {size}")

consolenames = {
    1: "DbgIn",
    2: "DbgOut",
    3: "Log"
}

def console_msg(dat, from_esp):
    idx = dat[0]
    msg = dat[1:].decode("ascii")

    print(f": console msg to {consolenames[idx]}: {msg!r}")

updcmds = {
    0x10: "CancelUpdate",
    0x11: "PepareForImage",
    0x30: "ImgReadError",
    0x31: "ImgCsumError",
    0x32: "InvalidStateError",
    0x40: "UpdateCompletedOk",
    0x50: "EspWroteSector",
    0x51: "EspCopying"
}

updstatuses = {
    0x10: "EnteredUpdMode",
    0x12: "ReadyForImage",
    0x13: "ReadyForChunk",
    0x20: "BeginningCopy",
    0x21: "CopyOk",
    0x30: "ResendLastChunkCsum",
    0x40: "AbortCsum",
    0x41: "AbortProtocol"
}

def update_statcmd(dat, from_esp, typ):
    code = dat[0]
    if typ:
        print(f": {'' if not from_esp else 'unexpected '} update status {updstatuses[code]}")
    else:
        print(f": {'' if from_esp else 'unexpected '} update command {updcmds[code]}")

def update_img_data(dat, from_esp):
    csum = struct.unpack("<H", dat[:2])[0]
    print(f": {len(dat)-2} bytes of image data with checksum {csum:04x}")

def update_img_start(dat, from_esp):
    csum, sz, cnt = struct.unpack("<HIH", dat)
    print(f": image header of length {sz} in {cnt} chunks with checksum {csum:04x} ")

phandle = {
    0x20: data_temp,
    0x30: ack_data_temp,
    0x41: query_time,
    0x21: lambda x, y: data_update(x, y, False),
    0x31: lambda x, y: ack_data_update(x, y, False),
    0x22: lambda x, y: data_update(x, y, True),
    0x32: lambda x, y: ack_data_update(x, y, True),
    0x23: data_del,
    0x33: ack_data_del,
    0x24: data_forgot,
    0x70: console_msg,
    0x60: lambda x, y: update_statcmd(x, y, False),
    0x63: lambda x, y: update_statcmd(x, y, True),
    0x61: update_img_data,
    0x62: update_img_start
}

while True:
    for pkt in read():
        if pkt[0] == 0xa6:
            print('E->S ', end='')
        elif pkt[0] == 0xa5:
            print('S->E ', end='')
        else:
            print(' UNK ', end='')

        if pkt[2] not in pnames:
            print(f": {pkt[0]:02x}{pkt[1]:02x}{pkt[2]:02x} <invalid packet>")
            continue

        print(": 0x{:02x} ({:20}) ".format(pkt[2], pnames[pkt[2]]), end="")

        if pkt[1] == 0x00:
            print()
            continue
        if pkt[1] == 0x01 and pkt[2] not in phandle:
            print(": {:02x}".format(pkt[3]))
        else:
            if pkt[2] in phandle:
                try:
                    phandle[pkt[2]](pkt[3:], pkt[0] == 0xa6)
                except:
                    print("error decoding " + binascii.hexlify(pkt[3:]).decode("ascii"))
            else:
                print("? " + binascii.hexlify(pkt[3:]).decode("ascii"))
