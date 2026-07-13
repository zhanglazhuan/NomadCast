"""Account management — simple JSON file storage.

Stores registered users with their unique ID: YYYYMMDDHHMM-XXXXX
where XXXXX is the first 5 chars of MD5(deviceID) hex digest (uppercase).
"""

import hashlib
import json
import os
from datetime import datetime
from pathlib import Path


def _hash_device_id(device_id: str) -> str:
    """Return first 5 uppercase hex chars of MD5(deviceID)."""
    return hashlib.md5(device_id.encode()).hexdigest()[:5].upper()

ACCOUNTS_FILE = Path(__file__).parent / "accounts.json"


def _load():
    if not ACCOUNTS_FILE.exists():
        return {}
    with open(ACCOUNTS_FILE, "r", encoding="utf-8") as f:
        return json.load(f)


def _save(data):
    with open(ACCOUNTS_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)


def register(name: str, password: str, device_id: str) -> dict:
    """Register a new user. Returns {"user_id": "20260710143052-ABCDEF"} or error."""
    data = _load()

    # Check if name already exists
    if name in data:
        # Already registered — verify password and return existing ID
        if data[name]["password"] == password:
            return {"user_id": data[name]["user_id"], "registered": False}
        else:
            return {"error": "Name already taken", "code": 409}

    # Generate unique ID: YYYYMMDDHHMM-XXXXX (5-char hash of deviceID)
    now = datetime.now()
    uid = now.strftime("%Y%m%d%H%M") + "-" + _hash_device_id(device_id)

    data[name] = {
        "password": password,
        "user_id": uid,
        "device_id": device_id,
        "created_at": now.isoformat(),
    }
    _save(data)
    return {"user_id": uid, "registered": True}


def login(name: str, password: str) -> dict:
    """Authenticate a user. Returns {"user_id": "..."} or error."""
    data = _load()

    if name not in data:
        return {"error": "User not found", "code": 404}

    if data[name]["password"] != password:
        return {"error": "Wrong password", "code": 401}

    return {"user_id": data[name]["user_id"]}
