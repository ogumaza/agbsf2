#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Recursive-descent Thumb/ARM disassembler for GBA ROMs.

It follows branches and calls from the given entry points, resolves PC-relative literal loads, and marks code that a
GSF rip zeroed out ("stripped"). gsfopt keeps only the bytes a song actually touched, so zeroed code is code the songs
never ran. It also marks where the walk runs into bytes that aren't an instruction ("undecodable"), such as data.

    gbadis.py ROM ENTRY [ENTRY ...] [--copy SRC:DST:SIZE]

Each ENTRY is an address in hex, with an A in front for ARM code. --copy maps SIZE bytes of the ROM at SRC to DST, all
in hex, for code that the game copies to RAM and runs there. For example, the driver's init and per-frame routine in
Donkey Kong Country (A5NE), and its sequencer at the address in IWRAM that the init copies it to:

    gbadis.py rom.gba 08032294 0803237c A030032b0 --copy 080311a8:03002b40:10a0
"""
import argparse
import re
import struct

from capstone import CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB, Cs
from capstone.arm import ARM_OP_IMM

from gbarom import ROM_BASE, load_rom

CONDITIONS = 'eq ne cs hs cc lo mi pl vs vc hi ls ge lt gt le al'.split()


class Disassembler:
    def __init__(self, rom, copies=()):
        # The memory the walk can read: each block of the ROM that the game copies elsewhere, then the ROM itself.
        self.regions = [(dst, rom[src - ROM_BASE:src - ROM_BASE + size]) for src, dst, size in copies]
        self.regions.append((ROM_BASE, rom))
        self.insns = {}   # address -> (insn, thumb)
        self.labels = {}
        self.literals = {}
        self.xrefs = {}
        self.stripped = {}  # address -> thumb, for the addresses the walk reached in code that a GSF rip zeroed out
        self.undecodable = {}  # address -> thumb, where the walk reached bytes that aren't an instruction
        self.md = {True: Cs(CS_ARCH_ARM, CS_MODE_THUMB), False: Cs(CS_ARCH_ARM, CS_MODE_ARM)}
        for md in self.md.values():
            md.detail = True

    def read(self, addr, size):
        """Returns the `size` bytes at addr, or None if they aren't all in one region."""
        for base, data in self.regions:
            if base <= addr and addr + size <= base + len(data):
                return data[addr - base:addr - base + size]
        return None

    def word(self, addr):
        return struct.unpack('<I', self.read(addr, 4))[0]

    def readable(self, addr, size=4):
        return self.read(addr, size) is not None

    def literal_address(self, ins, thumb):
        m = re.search(r'\[pc, #(-?0x[0-9a-f]+|-?\d+)\]', ins.op_str)
        pc = (ins.address + 4) & ~3 if thumb else ins.address + 8
        return pc + (int(m.group(1), 0) if m else 0)

    def run(self, entries):
        work = list(entries)
        while work:
            addr, thumb = work.pop()
            while addr not in self.insns and addr not in self.stripped and self.readable(addr, 2 if thumb else 4):
                # A Thumb instruction can end a region with 2 bytes.
                code = self.read(addr, 4) or self.read(addr, 2)
                if code == bytes(len(code)):
                    self.stripped[addr] = thumb
                    self.labels.setdefault(addr, 'STRIPPED_%08x' % addr)
                    break
                ins = next(self.md[thumb].disasm(code, addr), None)
                if ins is None:
                    self.undecodable[addr] = thumb
                    self.labels.setdefault(addr, 'BAD_%08x' % addr)
                    break
                self.insns[addr] = (ins, thumb)
                m = ins.mnemonic
                if m.startswith('ldr') and '[pc' in ins.op_str:
                    lit = self.literal_address(ins, thumb)
                    if self.readable(lit):
                        self.literals[lit] = self.word(lit)
                # ARM code has conditional calls too, such as bleq.
                is_call = m in ('bl', 'blx') or (m.startswith('bl') and m[2:] in CONDITIONS)
                is_branch = is_call or m == 'b' or (m.startswith('b') and m[1:] in CONDITIONS)
                if is_branch and ins.operands and ins.operands[0].type == ARM_OP_IMM:
                    target = ins.operands[0].imm
                    self.labels.setdefault(target, ('sub_%08x' if is_call else 'loc_%08x') % target)
                    self.xrefs.setdefault(target, []).append(addr)
                    work.append((target, (not thumb) if m == 'blx' else thumb))
                # An unconditional branch, or an instruction that loads pc, such as pop {pc}, ldmdb fp, {fp, sp, pc}
                # or subs pc, lr, #4.
                ends = (m in ('b', 'bx') or (m in ('pop', 'ldm', 'ldmib', 'ldmda', 'ldmdb') and 'pc' in ins.op_str)
                        or (m in ('mov', 'movs', 'add', 'adds', 'sub', 'subs', 'ldr') and ins.op_str.startswith('pc,')))
                if ends:
                    break
                addr += ins.size

    def listing(self):
        prev = None
        for addr in sorted(set(self.insns) | set(self.literals) | set(self.stripped) | set(self.undecodable)):
            if prev is not None and addr > prev:
                print('        ; gap %x bytes' % (addr - prev))
            if addr in self.labels:
                refs = self.xrefs.get(addr, [])
                callers = '   ; from ' + ' '.join('%x' % r for r in refs[:6]) if refs else ''
                print('\n%s:%s' % (self.labels[addr], callers))
            if addr in self.insns:
                ins, thumb = self.insns[addr]
                comment = ''
                if ins.mnemonic.startswith('ldr') and '[pc' in ins.op_str:
                    lit = self.literal_address(ins, thumb)
                    if self.readable(lit):
                        comment = ' ; =0x%08x' % self.word(lit)
                ops = ins.op_str
                if ins.operands and ins.operands[0].type == ARM_OP_IMM and ins.mnemonic.startswith('b'):
                    ops = self.labels.get(ins.operands[0].imm, ops)
                mode = '' if thumb else 'A:'
                print('%08x: %-10s %s%-7s %s%s' % (addr, ins.bytes.hex(), mode, ins.mnemonic, ops, comment))
                prev = addr + ins.size
            elif addr in self.undecodable:
                thumb = self.undecodable[addr]
                size = 2 if thumb else 4
                print('%08x: %-10s %s<undecodable>' % (addr, self.read(addr, size).hex(), '' if thumb else 'A:'))
                prev = addr + size
            elif addr in self.literals:
                print('%08x: .word 0x%08x' % (addr, self.literals[addr]))
                prev = addr + 4
            else:
                print('%08x: <stripped>' % addr)
                prev = addr + (2 if self.stripped[addr] else 4)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('rom', metavar='ROM', help='the game (.gba) or a GSF rip of it')
    p.add_argument('entries', metavar='ENTRY', nargs='+', help='an entry point in hex, with an A in front for ARM code')
    p.add_argument('--copy', action='append', default=[], metavar='SRC:DST:SIZE',
                   help='map SIZE bytes of the ROM at SRC to DST, all in hex')
    a = p.parse_args()
    try:
        copies = [tuple(int(v, 16) for v in c.split(':')) for c in a.copy]
    except ValueError:
        p.error('--copy takes SRC:DST:SIZE, in hex')
    if any(len(c) != 3 for c in copies):
        p.error('--copy takes SRC:DST:SIZE, in hex')
    dis = Disassembler(load_rom(a.rom), copies)
    entries = []
    for arg in a.entries:
        thumb = not arg.upper().startswith('A')
        try:
            addr = int(arg.lstrip('Aa'), 16) & ~1
        except ValueError:
            p.error('an entry point is an address in hex, with an A in front for ARM code: %s' % arg)
        dis.labels[addr] = 'sub_%08x' % addr
        entries.append((addr, thumb))
    dis.run(entries)
    dis.listing()


if __name__ == '__main__':
    main()
