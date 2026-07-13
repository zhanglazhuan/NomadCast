#!/usr/bin/env python3
"""Sync the project directory to the remote server via SFTP (rsync-like incremental).

Only transfers files that are new or changed. Skips common junk files.
"""

import hashlib
import stat
from pathlib import Path

import paramiko

REMOTE_USER = "zhanglazhuan"
REMOTE_HOST = "192.168.1.15"
REMOTE_PASS = "zhangla1991"
REMOTE_DIR = "~/Codes/Podcast"          # project dir on Pi; server/ → ~/Codes/Podcast/server

# Directory / file names to skip (exact match on name)
SKIP_PATTERNS = {
    "__pycache__",
    ".git",
    ".idea",
    ".vscode",
    ".DS_Store",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    "docs",                             # design docs / plans — not needed at runtime
    "debug",                            # debug scripts / test data / large media files
    "archive",                          # archived old code
    "scripts",                          # the sync script itself, no need on Pi
}

# File extensions to skip (with leading dot)
SKIP_EXTENSIONS = (
    ".pyc",
    ".pyo",
)

# Path prefixes to skip (directory trees to ignore entirely)
SKIP_PATH_PREFIXES = ()


def project_root() -> Path:
    return Path(__file__).resolve().parent.parent


def should_skip(path: Path) -> bool:
    name = path.name
    if name in SKIP_PATTERNS:
        return True
    if name.endswith(SKIP_EXTENSIONS):
        return True
    if name.startswith(".") and name != ".gitkeep":
        return True
    return False


def file_md5(path: Path) -> str:
    """Compute MD5 hash of a file."""
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def local_file_list(root: Path) -> dict[str, dict]:
    """Walk local project directory and return {rel_path: {size, mtime, md5}}."""
    files = {}
    for fpath in root.rglob("*"):
        if fpath.is_file():
            rel = str(fpath.relative_to(root)).replace("\\", "/")
            # Check if any parent dir should be skipped
            skip = False
            for p in fpath.relative_to(root).parents:
                if should_skip(Path(p)):
                    skip = True
                    break
            if skip or should_skip(fpath):
                continue
            if rel.startswith(SKIP_PATH_PREFIXES):
                continue
            files[rel] = {
                "size": fpath.stat().st_size,
                "mtime": int(fpath.stat().st_mtime),
                "md5": file_md5(fpath),
            }
    return files


def remote_file_list(sftp: paramiko.SFTPClient, remote_root: str) -> dict[str, dict]:
    """Walk remote directory via SFTP and return {rel_path: {size, mtime}}."""
    files = {}

    def _walk(remote_dir: str, prefix: str):
        try:
            for entry in sftp.listdir_attr(remote_dir):
                name = entry.filename
                full = f"{remote_dir}/{name}"
                rel = f"{prefix}{name}"
                if stat.S_ISDIR(entry.st_mode or 0):
                    if not should_skip(Path(name)):
                        _walk(full, f"{rel}/")
                else:
                    if not should_skip(Path(name)):
                        files[rel] = {"size": entry.st_size or 0, "mtime": entry.st_mtime or 0}
        except OSError:
            pass

    _walk(remote_root, "")
    return files


def resolve_remote_home(ssh: paramiko.SSHClient) -> str:
    """Resolve the remote home directory path."""
    _, stdout, _ = ssh.exec_command("echo $HOME")
    return stdout.read().decode().strip()


def ensure_remote_dir(sftp: paramiko.SFTPClient, remote_path: str) -> None:
    """Create remote directory and all parents if they don't exist."""
    remote_path = remote_path.replace("\\", "/")
    parts = [p for p in remote_path.strip("/").split("/") if p]
    current = ""
    for p in parts:
        current = f"{current}/{p}"
        try:
            sftp.stat(current)
        except OSError:
            try:
                sftp.mkdir(current)
            except OSError:
                pass  # race condition — another process created it


def main() -> None:
    root = project_root()
    project_name = root.name
    remote_root = f"{REMOTE_DIR}/{project_name}"

    print(f"==> Scanning local files...")
    local = local_file_list(root)
    print(f"    {len(local)} local files")

    print(f"==> Connecting to {REMOTE_USER}@{REMOTE_HOST}...")
    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(REMOTE_HOST, username=REMOTE_USER, password=REMOTE_PASS,
                look_for_keys=False, allow_agent=False)

    sftp = ssh.open_sftp()

    # Resolve ~ in remote path via the shell
    remote_home = resolve_remote_home(ssh)
    remote_root = remote_root.replace("~", remote_home)

    # Ensure remote root exists
    try:
        sftp.stat(remote_root)
    except OSError:
        ensure_remote_dir(sftp, remote_root)

    print(f"==> Scanning remote files...")
    remote = remote_file_list(sftp, remote_root)
    print(f"    {len(remote)} remote files")

    # Determine what to transfer
    to_upload = []
    for rel, info in local.items():
        r = remote.get(rel)
        if r is None:
            to_upload.append((rel, "new"))
        elif r["size"] != info["size"] or abs(r["mtime"] - info["mtime"]) > 2:
            to_upload.append((rel, "changed"))
        # unchanged — skip

    # Files on remote but not local — delete (keep server-side logs untouched)
    to_delete = [rel for rel in remote if rel not in local and not rel.startswith(SKIP_PATH_PREFIXES)]

    print(f"==> Changes: {len(to_upload)} to upload, {len(to_delete)} to delete")

    if to_upload:
        for rel, reason in to_upload:
            local_path = str(root / rel).replace("\\", "/")
            remote_path = f"{remote_root}/{rel}"
            parent = "/".join(remote_path.replace("\\", "/").split("/")[:-1])
            ensure_remote_dir(sftp, parent)
            try:
                sftp.put(local_path, remote_path)
                # Preserve local mtime so subsequent syncs don't re-transfer
                local_mtime = local[rel]["mtime"]
                sftp.utime(remote_path, (local_mtime, local_mtime))
            except Exception as e:
                print(f"    FAIL {rel}: {e}")
                continue
            print(f"    [{reason:7s}] {rel}")

    for rel in to_delete:
        try:
            sftp.remove(f"{remote_root}/{rel}")
            print(f"    [deleted ] {rel}")
        except OSError as e:
            print(f"    FAIL delete {rel}: {e}")

    sftp.close()
    ssh.close()

    summary = f"up={len(to_upload)} del={len(to_delete)} unchanged={len(local) - len(to_upload)}"
    print(f"==> Sync complete: {remote_root} ({summary})")


if __name__ == "__main__":
    main()
