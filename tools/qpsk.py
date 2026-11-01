# Python 3 port of Mutable Instruments' stm_audio_bootloader QPSK encoder
# (MIT, Emilie Gillet), plus a matching decoder used to check the output.
import struct, sys, zlib
import numpy as np

SR, CARRIER, BAUD, PACKET = 48000, 6000, 12000, 256
PAGE, PAUSE = 2048, 0.15  # stm32f3

class Scrambler:
    def __init__(self, seed=0): self.s = seed
    def run(self, data):
        out = bytearray(data)
        for k in range(len(out)):
            out[k] ^= self.s >> 24
            self.s = (self.s * 1664525 + 1013904223) & 0xffffffff
        return bytes(out)

class Encoder:
    def __init__(self):
        self.idx = 0
        self.scr = Scrambler(0)
    def _encode(self, syms):
        ratio = SR // BAUD * 2
        syms = np.asarray(syms, dtype=int)
        even = (2 * np.repeat(syms % 2, ratio) - 1) / np.sqrt(2.0)
        odd = (2 * np.repeat(syms // 2, ratio) - 1) / np.sqrt(2.0)
        t = (np.arange(len(even), dtype=float) + self.idx) / SR
        self.idx += len(even)
        ph = 2 * np.pi * CARRIER * t
        return even * np.sin(ph) + odd * np.cos(ph)
    def blank(self, dur):
        return self._encode(np.zeros(int(dur * BAUD / 8) * 4, dtype=int))
    def packet(self, data):
        data = data + b'\xff' * (PACKET - len(data))
        data = self.scr.run(data)
        crc = zlib.crc32(data) & 0xffffffff
        b = [0] * 8 + [0x99] * 4 + [0xcc] * 4 + list(data) + list(crc.to_bytes(4, 'big'))
        syms = [(x >> s) & 3 for x in b for s in (6, 4, 2, 0)]
        return self._encode(syms)

def encode(bin_path, wav_path):
    data = open(bin_path, 'rb').read()
    if len(data) % PAGE:
        data += b'\xff' * (PAGE - len(data) % PAGE)
    enc = Encoder()
    blocks = [np.zeros(SR), enc.blank(1.0)]
    n = 0
    for off in range(0, len(data), PACKET):
        blocks.append(enc.packet(data[off:off + PACKET]))
        n += 1
        if n == PAGE // PACKET:
            blocks.append(enc.blank(PAUSE)); n = 0
    blocks.append(enc.blank(1.0))
    sig = np.clip(np.concatenate(blocks), -1, 1)
    pcm = (sig * 32767).astype('<i2').tobytes()
    with open(wav_path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<L', 36 + len(pcm)) + b'WAVEfmt ')
        f.write(struct.pack('<LHHLLHH', 16, 1, 1, SR, SR * 2, 2, 16))
        f.write(b'data' + struct.pack('<L', len(pcm)) + pcm)

def decode(wav_path):
    raw = open(wav_path, 'rb').read()
    pos = 12; fmt = None
    while pos < len(raw):
        cid, size = raw[pos:pos+4], struct.unpack('<L', raw[pos+4:pos+8])[0]
        body = raw[pos+8:pos+8+size]
        if cid == b'fmt ': fmt = struct.unpack('<HHLLHH', body[:16])
        if cid == b'data': pcm = body
        pos += 8 + size + (size & 1)
    assert fmt[1] == 1 and fmt[2] == SR and fmt[5] == 16, fmt
    x = np.frombuffer(pcm, '<i2').astype(float)[SR:]  # skip 1 s of silence
    per = SR // CARRIER
    x = x[:len(x) // per * per].reshape(-1, per)
    ph = 2 * np.pi * np.arange(per) / per
    q = x @ np.sin(ph); i = x @ np.cos(ph)
    syms = (q > 0).astype(int) + 2 * (i > 0).astype(int)
    scr = Scrambler(0); out = bytearray(); k = 0; good = bad = 0
    while True:
        nz = np.nonzero(syms[k:])[0]
        if not len(nz): break
        k += nz[0]
        s = syms[k:k + 4 * (8 + PACKET + 4)]
        if len(s) < 4 * (8 + PACKET + 4): break
        by = bytes((s[j]<<6)|(s[j+1]<<4)|(s[j+2]<<2)|s[j+3] for j in range(0, len(s), 4))
        assert by[:8] == b'\x99'*4 + b'\xcc'*4, by[:8].hex()
        body, crc = by[8:8+PACKET], int.from_bytes(by[8+PACKET:], 'big')
        if zlib.crc32(body) & 0xffffffff == crc: good += 1
        else: bad += 1
        out += scr.run(body)
        k += len(s)
    return bytes(out), good, bad

if __name__ == '__main__':
    if sys.argv[1] == 'encode': encode(sys.argv[2], sys.argv[3])
    else:
        data, good, bad = decode(sys.argv[2])
        open(sys.argv[3], 'wb').write(data)
        print(f'packets ok={good} bad={bad} bytes={len(data)}')
