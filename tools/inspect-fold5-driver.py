#!/usr/bin/env python3
"""Inspect the shipped ARM64 ELF relocations without loading/modifying a driver."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(path):
    b = path.read_bytes()
    if b[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<HH', b, 16) != (1, 183):
        raise ValueError('Expected an ARM64 little-endian ELF64 relocatable module')
    off = struct.unpack_from('<Q', b, 40)[0]
    width, count = struct.unpack_from('<HH', b, 58)
    if width != 64 or off + count * width > len(b):
        raise ValueError('Invalid section table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', b, off + i * width) for i in range(count)]
    tables = {}
    for i, sec in enumerate(sections):
        if sec[1] != 2:
            continue
        strings = sections[sec[6]]
        st = b[strings[4]:strings[4] + strings[5]]
        symbols = []
        if sec[9] != 24 or sec[4] + sec[5] > len(b):
            raise ValueError('Invalid symbol table')
        for pos in range(sec[4], sec[4] + sec[5], sec[9]):
            n, info, other, section, value, size = struct.unpack_from('<IBBHQQ', b, pos)
            end = st.find(b'\0', n)
            if end < n:
                raise ValueError('Invalid symbol name')
            symbols.append({'name': st[n:end].decode(), 'section': section, 'value': value, 'size': size})
        tables[i] = symbols
    def callbacks(table, symbols):
        result = {}
        for sec in sections:
            if sec[1] != 4 or sec[7] != table['section']:
                continue
            if sec[9] != 24 or sec[4] + sec[5] > len(b):
                raise ValueError('Invalid relocation table')
            linked = tables[sec[6]]
            for pos in range(sec[4], sec[4] + sec[5], 24):
                target, info, addend = struct.unpack_from('<QQq', b, pos)
                if not table['value'] <= target < table['value'] + table['size']:
                    continue
                symbol = linked[info >> 32]
                candidates = [s['name'] for s in symbols if s['section'] == symbol['section'] and
                              s['value'] == symbol['value'] + addend and s['name'].endswith('.cfi_jt')]
                result[target - table['value']] = candidates or [symbol['name'] + '+' + hex(addend)]
        return result
    for symbols in tables.values():
        monitor = next((s for s in symbols if s['name'] == 'wlan_mon_drv_ops'), None)
        if monitor is None:
            continue
        mon = callbacks(monitor, symbols)
        for regular in symbols:
            if not regular['name'].startswith('wlan_drv_ops') or regular['size'] != monitor['size']:
                continue
            normal = callbacks(regular, symbols)
            tx = [at for at, names in normal.items() if any('hdd_hard_start_xmit' in n for n in names)]
            if len(tx) != 1:
                continue
            offset = tx[0]
            data = sections[monitor['section']]
            at = data[4] + monitor['value'] + offset
            raw = b[at:at + 8]
            return {'sha256': hashlib.sha256(b).hexdigest(), 'monitor_table': monitor,
                    'station_table': regular, 'monitor_callbacks': {hex(k): v for k, v in mon.items()},
                    'station_tx_callback_offset': hex(offset), 'monitor_tx_field_hex': raw.hex(),
                    'monitor_tx_is_null': len(raw) == 8 and raw == b'\0' * 8 and offset not in mon,
                    'interpretation': 'A NULL monitor ndo_start_xmit provides no standard monitor packet TX path. Other firmware or management-frame paths are not ruled out.'}
    raise ValueError('Could not identify matching station and monitor network operation tables')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('driver', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.driver)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
