#!/usr/bin/env python3
"""Read-only V4 evidence inspector. Never opens the game or follows stored pointers.
Prints a JSON report to stdout only, after all checksums/EOF have passed.
This is NOT the native restore validator. Python 3, standard library only.
"""
import argparse
import hashlib
import json
import pathlib
import struct

CAP = 0xA10000
PREFIXES = (0x108, 0x4668, 0x2180, 0x1E88)

class Reader:
    def __init__(self, data):
        self.data, self.pos, self.block = data, 0, 0
    def take(self, size):
        if size < 0 or size > len(self.data) - self.pos:
            raise ValueError('truncated archive')
        start = self.pos
        self.pos += size
        return self.data[start:self.pos]
    def u32(self):
        return struct.unpack('<I', self.take(4))[0]
    def u64(self):
        return struct.unpack('<Q', self.take(8))[0]
    def text(self, limit=4096):
        size = self.u32()
        if size > limit:
            raise ValueError('text too long')
        return self.take(size).decode('utf-8', errors='backslashreplace')
    def check(self):
        h = 2166136261
        for byte in memoryview(self.data)[self.block:self.pos]:
            h = ((h ^ byte) * 16777619) & 0xffffffff
        if self.u32() != h:
            raise ValueError('checksum mismatch')
        self.block = self.pos

def words(data):
    return list(struct.unpack('<' + 'I' * (len(data) // 4), data))

def base(r):
    frame, first, second_off, second = [r.u32() for _ in range(4)]
    manager, buffer = r.u64(), r.u64()
    slot, epoch, serial = r.u32(), r.u64(), r.u64()
    if not (0 < first <= second_off <= CAP and 0 < second <= CAP - second_off and slot < 10):
        raise ValueError('invalid main layout')
    descriptor, queue = words(r.take(72)), words(r.take(1024))
    context = r.u32()
    auxiliary = []
    for i, expected in enumerate(PREFIXES):
        offset, address, size = r.u32(), r.u32(), r.u32()
        if offset != (0x174 + i * 4) or size != expected:
            raise ValueError('unsupported auxiliary prefix')
        prefix = r.take(size)
        payload_addr, size = r.u32(), r.u32()
        if size > (0x200000 if i == 0 else 0):
            raise ValueError('auxiliary payload too large')
        payload = r.take(size)
        auxiliary.append(dict(contextOffset=hex(offset), address=hex(address),
            prefixWords=words(prefix), payloadAddress=hex(payload_addr), payloadBytes=size,
            payloadSHA256=hashlib.sha256(payload).hexdigest()))
    evidence, details = r.text(256 * 1024), r.text()
    size = r.u32()
    if size != first + second or size > CAP:
        raise ValueError('payload size mismatch')
    payload = r.take(size)
    r.check()
    return dict(frame=frame, firstSize=first, secondOffset=second_off, secondSize=second,
        sourceManager=hex(manager), sourceBuffer=hex(buffer), slot=slot, epoch=epoch, serial=serial,
        descriptorWords=descriptor, queueWords=queue, context=hex(context), auxiliary=auxiliary,
        runtimeEvidence=evidence, details=details, payloadSHA256=hashlib.sha256(payload).hexdigest())

def inspect(data):
    r = Reader(data)
    if r.take(17) != b'BBCF_TAS_PROJECT\0' or (r.u32(), r.u32(), r.u32()) != (4, 1, 60):
        raise ValueError('expected archive-only V4, lead-in 60')
    compatibility = r.text()
    r.check()
    a, b = base(r), base(r)
    if b['frame'] - a['frame'] != 60 or a['sourceManager'] != b['sourceManager'] or a['slot'] == b['slot']:
        raise ValueError('invalid base pair')
    count, cursor = r.u32(), r.u32()
    if not 0 < count <= 1000000 or cursor > count:
        raise ValueError('invalid movie count/cursor')
    movie = r.take(count * 4)
    for (packed,) in struct.iter_unpack('<I', movie):
        for value in (packed & 0xffff, packed >> 16):
            if not 1 <= (value & 15) <= 9 or value & ~0x1ff:
                raise ValueError('invalid input')
    n = r.u32()
    if n > 4096:
        raise ValueError('too many sections')
    sections, previous = [], -1
    for _ in range(n):
        frame, name = r.u32(), r.text()
        if not previous < frame <= count or not name:
            raise ValueError('invalid section')
        sections.append(dict(frame=frame, name=name))
        previous = frame
    r.check()
    if r.pos != len(data):
        raise ValueError('trailing bytes')
    return dict(archiveSHA256=hashlib.sha256(data).hexdigest(), version=4,
        nativeLoadAllowed=False, validation='encoding-checksums-and-basic-layout-only',
        compatibility=compatibility, A=a, B=b, frames=count, cursor=cursor, sections=sections,
        movieSHA256=hashlib.sha256(movie).hexdigest())

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=pathlib.Path)
    args = parser.parse_args()
    try:
        with args.archive.open('rb') as stream:
            data = stream.read(48 * 1024 * 1024 + 1)
        if len(data) > 48 * 1024 * 1024:
            raise ValueError('archive exceeds inspector size limit')
        report = inspect(data)
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, 'ERROR: ' + str(error) + '\n')
    print(json.dumps(report, ensure_ascii=True, indent=2))

if __name__ == '__main__':
    main()
