# SPDX-License-Identifier: MIT
# Counts the events of every MIDI file that a song record of WarioWare or Rhythm Tengoku points to.
#   smfscan.py ROM SONG_TABLE
import struct, sys, collections
r = open(sys.argv[1], 'rb').read()
O = 0x08000000
T = int(sys.argv[2], 16)
def u32(a): return struct.unpack_from('<I', r, a - O)[0]
def be32(o): return struct.unpack_from('>I', r, o)[0]
def be16(o): return struct.unpack_from('>H', r, o)[0]
recs = []
i = 0
while True:
    a = T + 8 * i
    rec = u32(a)
    if rec == 0:
        i += 1
        if i > 4000: break
        continue
    if not (O <= rec < O + len(r)) or not (O <= u32(rec) < O + len(r)) or r[u32(rec) - O:u32(rec) - O + 4] != b'MThd':
        break
    recs.append((i, rec)); i += 1
cc = collections.Counter(); ccm = collections.Counter(); meta = collections.Counter(); kinds = collections.Counter()
sysex = collections.Counter(); fmt = collections.Counter(); progs = collections.Counter(); markers = collections.Counter()
cc_songs = collections.defaultdict(set)
for idx, rec in recs:
    o = u32(rec) - O
    hl = be32(o + 4); f = be16(o + 8); nt = be16(o + 10); div = be16(o + 12)
    fmt[(f, div)] += 1
    p = o + 8 + hl
    music = True
    for t in range(nt):
        assert r[p:p + 4] == b'MTrk', (idx, hex(p))
        ln = be32(p + 4); q = p + 8; end = q + ln; st = 0
        while q < end:
            v = 0
            while True:
                b = r[q]; q += 1; v = (v << 7) | (b & 0x7F)
                if b < 0x80: break
            b = r[q]
            if b & 0x80: st = b; q += 1
            hi = st & 0xF0
            if st == 0xFF:
                ty = r[q]; q += 1
                v = 0
                while True:
                    b = r[q]; q += 1; v = (v << 7) | (b & 0x7F)
                    if b < 0x80: break
                meta[ty] += 1
                if ty == 6: markers[r[q:q + v]] += 1
                q += v
                if ty == 0x2F: break
            elif st in (0xF0, 0xF7):
                v = 0
                while True:
                    b = r[q]; q += 1; v = (v << 7) | (b & 0x7F)
                    if b < 0x80: break
                sysex[r[q:q + v].hex()] += 1; q += v
            elif hi in (0x80, 0x90, 0xA0, 0xB0, 0xE0):
                if hi == 0xB0: cc[r[q]] += 1; cc_songs[r[q]].add(idx); ccm[(r[q], r[q + 1])] += 1
                kinds[hi] += 1; q += 2
            elif hi in (0xC0, 0xD0):
                kinds[hi] += 1
                if hi == 0xC0: progs[r[q]] += 1
                q += 1
            else:
                raise SystemExit('bad status %x at %x' % (st, q))
        p = end
print('songs', len(recs), 'formats', fmt)
print('kinds', {hex(k): v for k, v in kinds.items()})
print('meta', {hex(k): v for k, v in meta.items()}, 'markers', markers)
print('sysex', sysex.most_common(10))
print('cc', sorted((k, v, len(cc_songs[k])) for k, v in cc.items()))
for k in sorted(cc):
    vals = sorted({v for (c, v) in ccm if c == k})
    print('  cc', k, 'values', vals if len(vals) < 20 else (len(vals), vals[:5], vals[-5:]))
