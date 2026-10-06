"""Account management — simple JSON file storage.

Stores registered users with their unique ID: YYYYMMDDHHMM-XXXXX
where XXXXX is the first 5 chars of MD5(deviceID) hex digest (uppercase).

Passwords are stored as Werkzeug hashes (PBKDF2 by default), never in plaintext.
Legacy accounts with a plaintext password are transparently upgraded to a hash
on the next successful login/register, so existing accounts.json files keep
working without a reset.
"""

import hashlib
import json
import threading
from datetime import datetime
from pathlib import Path

from werkzeug.security import generate_password_hash, check_password_hash


def _hash_device_id(device_id: str) -> str:
    """Return first 5 uppercase hex chars of MD5(deviceID)."""
    return hashlib.md5(device_id.encode()).hexdigest()[:5].upper()


ACCOUNTS_FILE = Path(__file__).parent / "accounts.json"

# One lock around load → modify → save. register/login are read-modify-write;
# without this, concurrent requests can clobber each other's writes.
_ACCOUNTS_LOCK = threading.Lock()


def _load():
    if not ACCOUNTS_FILE.exists():
        return {}
    with open(ACCOUNTS_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def _save(data):
    with open(ACCOUNTS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def _is_hashed(password: str) -> bool:
    """Werkzeug hashes carry a method prefix like `pbkdf2:` / `scrypt:`."""
    return password.startswith(("pbkdf2:", "scrypt:"))


def _verify(stored: str, password: str) -> bool:
    """Compare `password` against `stored`, which may be a hash or legacy plaintext."""
    if _is_hashed(stored):
        return check_password_hash(stored, password)
    return stored == password


def register(name: str, password: str, device_id: str) -> dict:
    """Register a new user. Returns {"user_id": "20260710143052-ABCDEF"} or error."""
    with _ACCOUNTS_LOCK:
        data = _load()

        # Check if name already exists
        if name in data:
            if _verify(data[name]["password"], password):
                # Lazy-migrate a legacy plaintext password to a hash.
                if not _is_hashed(data[name]["password"]):
                    data[name]["password"] = generate_password_hash(password)
                    _save(data)
                return {"user_id": data[name]["user_id"], "registered": False}
            return {"error": "Name already taken", "code": 409}

        # Generate unique ID: YYYYMMDDHHMM-XXXXX (5-char hash of deviceID)
        now = datetime.now()
        uid = now.strftime("%Y%m%d%H%M") + "-" + _hash_device_id(device_id)

        data[name] = {
            "password": generate_password_hash(password),
            "user_id": uid,
            "device_id": device_id,
            "created_at": now.isoformat(),
        }
        _save(data)
        return {"user_id": uid, "registered": True}


def login(name: str, password: str) -> dict:
    """Authenticate a user. Returns {"user_id": "..."} or error."""
    with _ACCOUNTS_LOCK:
        data = _load()

        if name not in data:
            return {"error": "User not found", "code": 404}

        if not _verify(data[name]["password"], password):
            return {"error": "Wrong password", "code": 401}

        # Lazy-migrate a legacy plaintext password to a hash.
        if not _is_hashed(data[name]["password"]):
            data[name]["password"] = generate_password_hash(password)
            _save(data)

        return {"user_id": data[name]["user_id"]}
