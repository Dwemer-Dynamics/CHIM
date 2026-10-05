"""Add the two CHIM Interact records without rewriting unrelated ESP bytes.

Run from any directory. The maintained ESP is the source asset; this authoring
script documents and verifies its narrow, idempotent construction.
"""
from pathlib import Path
import struct

PATH = Path(__file__).resolve().parents[2] / 'AIAgent' / 'AIAgent.esp'
SPELL = 0x0505A0F1
EFFECT = 0x0505A0F2

def sub(tag, value):
    return tag + struct.pack('<H', len(value)) + value

def record(tag, form, payload):
    return tag + struct.pack('<III', len(payload), 0, form) + b'\0' * 4 + struct.pack('<HH', 44, 0) + payload

def chunks(data):
    pos = 0
    while pos < len(data):
        size = struct.unpack_from('<H', data, pos + 4)[0]
        yield data[pos:pos+4], data[pos+6:pos+6+size]
        pos += 6 + size
    assert pos == len(data)

def walk(data, start=0, end=None):
    end = len(data) if end is None else end
    while start < end:
        tag = data[start:start+4]
        size = struct.unpack_from('<I', data, start+4)[0]
        if tag == b'GRUP':
            yield from walk(data, start+24, start+size)
            start += size
        else:
            yield tag, struct.unpack_from('<I', data, start+12)[0], data[start+24:start+24+size]
            start += 24+size
    assert start == end

data = PATH.read_bytes()
records = list(walk(data))
ids = {fid for _, fid, _ in records}
if SPELL in ids or EFFECT in ids:
    assert SPELL in ids and EFFECT in ids, 'Partial Interact record installation'
    assert any(tag == b'SPEL' and fid == SPELL and b'CHIMInteractPower\0' in body for tag, fid, body in records)
    assert any(tag == b'MGEF' and fid == EFFECT and b'CHIMInteractEffect\0' in body for tag, fid, body in records)
    print('CHIM Interact records already present; no changes')
else:
    # Use the existing script-archetype effect layout, without its script or flags.
    template = next(body for tag, fid, body in records if tag == b'MGEF' and fid == 0x0502839A)
    effect_data = bytearray(dict(chunks(template))[b'DATA'])
    assert len(effect_data) == 152
    struct.pack_into('<I', effect_data, 0, (1 << 10) | (1 << 11) | (1 << 15) | (1 << 27))
    effect = record(b'MGEF', EFFECT, sub(b'EDID', b'CHIMInteractEffect\0') + sub(b'FULL', b'CHIM Interact\0')
                    + sub(b'DATA', effect_data) + sub(b'DNAM', b'\0'))
    # Cost zero, manual cost, repeatable lesser power, fire-and-forget, self.
    spit = struct.pack('<IIIfIIffI', 0, 1, 3, 0.0, 1, 0, 0.0, 0.0, 0)
    spell = record(b'SPEL', SPELL, sub(b'EDID', b'CHIMInteractPower\0') + sub(b'OBND', b'\0'*12)
                   + sub(b'FULL', b'CHIM Interact\0') + sub(b'ETYP', struct.pack('<I', 0x25BEE))
                   + sub(b'DESC', b'Use an inventory item on the target in your crosshair.\0')
                   + sub(b'SPIT', spit) + sub(b'EFID', struct.pack('<I', EFFECT))
                   + sub(b'EFIT', struct.pack('<fII', 0.0, 0, 1)))
    additions = {b'MGEF': effect, b'SPEL': spell}
    output = bytearray()
    pos = 0
    while pos < len(data):
        tag = data[pos:pos+4]
        size = struct.unpack_from('<I', data, pos+4)[0]
        total = size if tag == b'GRUP' else size+24
        block = bytearray(data[pos:pos+total])
        if tag == b'TES4':
            # HEDR is the first subrecord in this maintained asset.
            assert block[24:28] == b'HEDR'
            count, next_id = struct.unpack_from('<II', block, 34)
            # Preserve stable Interact IDs after rebases; never move the upstream allocation counter backwards.
            struct.pack_into('<II', block, 34, count+2, max(next_id, (EFFECT & 0xFFFFFF)+1))
        elif tag == b'GRUP' and bytes(block[8:12]) in additions and struct.unpack_from('<I', block, 12)[0] == 0:
            block += additions.pop(bytes(block[8:12]))
            struct.pack_into('<I', block, 4, len(block))
        output += block
        pos += total
    assert not additions, 'Expected top-level record groups missing'
    updated = list(walk(bytes(output)))
    assert len(updated) == len(records)+2
    original = {(tag,fid): body for tag,fid,body in records if tag != b'TES4'}
    assert all(original[(tag,fid)] == body for tag,fid,body in updated if (tag,fid) in original)
    PATH.write_bytes(output)
    print('Added Interact SPEL 0505A0F1 and MGEF 0505A0F2; all other records preserved')

# Verify the authored power cannot inherit another effect's script or visual behavior.
final_records = list(walk(PATH.read_bytes()))
effect_fields = dict(chunks(next(body for tag, fid, body in final_records if fid == EFFECT)))
spell_fields = dict(chunks(next(body for tag, fid, body in final_records if fid == SPELL)))
assert b'VMAD' not in effect_fields
assert struct.unpack_from('<I', effect_fields[b'DATA'], 64)[0] == 1  # Script archetype
assert struct.unpack_from('<II', effect_fields[b'DATA'], 80) == (1, 0)  # FireAndForget/Self
assert struct.unpack_from('<I', spell_fields[b'SPIT'], 8)[0] == 3  # LesserPower
assert struct.unpack_from('<II', spell_fields[b'SPIT'], 16) == (1, 0)
# On-disk DATA form-reference fields (associated item, visuals, projectile, ability, perk).
for offset in [8, 24, 32, 36, 72, 76, 92, 96, 100, 108, 116, 120, 124, 128, 132, 136]:
    assert struct.unpack_from('<I', effect_fields[b'DATA'], offset)[0] == 0
