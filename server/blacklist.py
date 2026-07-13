"""Channel blacklist — stores collection IDs whose feeds are unparseable.

When /api/episodes fails with a ValueError, the channel's collection_id
is automatically added. The /api/charts/full endpoint filters them out.
"""

import json
from pathlib import Path

BLACKLIST_FILE = Path(__file__).parent / "blacklist.json"


def _load() -> set:
    if not BLACKLIST_FILE.exists():
        return set()
    with open(BLACKLIST_FILE, "r") as f:
        return set(json.load(f))


def _save(data: set):
    # Filter out old URL-format entries (migration from feed_url → collection_id)
    ids = sorted([x for x in data if isinstance(x, int)])
    with open(BLACKLIST_FILE, "w") as f:
        json.dump(ids, f, indent=2)


def is_blacklisted(collection_id: int) -> bool:
    return collection_id in _load()


def add(collection_id: int):
    data = _load()
    data.add(collection_id)
    _save(data)


def remove(collection_id: int):
    data = _load()
    data.discard(collection_id)
    _save(data)


def all_ids() -> list:
    return sorted(list(_load()))
