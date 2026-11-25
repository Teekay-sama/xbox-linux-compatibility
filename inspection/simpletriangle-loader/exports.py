"""Read a PE export table without loading the image."""

import struct


MAX_EXPORTS = 200_000
EXPORT_DIRECTORY = '<IIHHIIIIIII'


class ExportFormatError(ValueError):
    """The export directory is incomplete or has an invalid layout."""


def _table(pe, rva, count, fmt, label):
    """Iterate a fixed-width export array within one file-backed region."""
    if not count:
        return iter(())
    if not rva:
        raise ExportFormatError(f'Missing {label} table RVA')
    width = struct.calcsize(fmt)
    offset, available = pe.mapped(rva)
    size = count * width
    if size > available or rva + size > 1 << 32:
        raise ExportFormatError(f'Truncated {label} table')
    return (value for (value,) in struct.iter_unpack(fmt, pe.data[offset:offset + size]))


def _forwarder(pe, rva, directory_end):
    offset, available = pe.mapped(rva)
    limit = offset + min(directory_end - rva, available, 4096)
    end = pe.data.find(b'\0', offset, limit)
    if end <= offset:
        raise ExportFormatError(f'Missing or unterminated forwarder at RVA {rva:#x}')
    return pe.data[offset:end].decode('ascii', errors='backslashreplace')


def read_exports(pe):
    """Return case-sensitive names and public ordinals for the supplied PE.

    A nonzero address establishes an export entry, not its calling convention
    or behavior. Forwarder targets are recorded as strings and left unresolved.
    """
    result = {'names': {}, 'ordinals': {}}
    rva, size = pe.directory(0)
    if not rva and not size:
        return result
    header_size = struct.calcsize(EXPORT_DIRECTORY)
    directory_end = rva + size
    if not rva or size < header_size or directory_end > 1 << 32:
        raise ExportFormatError('Invalid export directory bounds')
    header, available = pe.mapped(rva)
    if available < header_size:
        raise ExportFormatError('Truncated export directory header')

    (_, _, _, _, _, ordinal_base, function_count, name_count,
     functions_rva, names_rva, indices_rva) = pe.read(EXPORT_DIRECTORY, header)
    if function_count > MAX_EXPORTS or name_count > MAX_EXPORTS:
        raise ExportFormatError('Export count exceeds inspection limits')
    if ordinal_base + function_count > 1 << 32:
        raise ExportFormatError('Export ordinal range exceeds 32 bits')

    ordinals = result['ordinals']
    for index, address in enumerate(_table(pe, functions_rva, function_count, '<I', 'address')):
        if not address:
            continue
        ordinals[ordinal_base + index] = (
            _forwarder(pe, address, directory_end) if rva <= address < directory_end else None
        )

    names = result['names']
    name_rvas = _table(pe, names_rva, name_count, '<I', 'name pointer')
    indices = _table(pe, indices_rva, name_count, '<H', 'ordinal index')
    for name_rva, index in zip(name_rvas, indices):
        ordinal = ordinal_base + index
        if index >= function_count:
            raise ExportFormatError(f'Export name has out-of-range ordinal index {index}')
        if ordinal not in ordinals:
            continue
        if not name_rva:
            raise ExportFormatError('Export name points to RVA zero')
        name = pe.c_string(name_rva)
        if name in names and names[name] != ordinal:
            raise ExportFormatError(f'Conflicting ordinals for export {name!r}')
        names[name] = ordinal
    return result
