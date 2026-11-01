"""Base game selection for cartridge dumps that bundle an update (a "combined" XCI).

The vendored program_nca() accepts one content set only. A combined XCI carries the base set and an update set (its
Meta NCA under the update title ID, its Program NCA a BKTR patch). base_records() drops the update set so the base
checks run unchanged; install() puts that in front of the vendored program_nca() for scripts run from it."""
import extract_exefs

BASE_TITLE = '0100453019AA8000'
_vendor_program_nca = extract_exefs.program_nca


def is_bktr(hdr):
    return any(hdr[0x400 + i * 512 + 4] == 4 for i in range(4))


def base_records(records):
    """records minus a bundled update: Meta NCAs of another title and BKTR Program NCAs. A package with no base
    Meta NCA is left as it is, so an update or another game still fails the vendored checks."""
    kept = [(r, h) for r, h in records
            if not (r['content_type'] == 1 and r['title_id'] != BASE_TITLE)
            and not (r['content_type'] == 0 and is_bktr(h))]
    if not any(r['content_type'] == 1 for r, _ in kept):
        return records
    return kept


def program_nca(records, update=False):
    return _vendor_program_nca(records if update else base_records(records), update=update)


def install():
    extract_exefs.program_nca = program_nca
