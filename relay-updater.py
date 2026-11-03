#!/usr/bin/env python3
"""Relay startup updater: authenticated, isolated Linux x86-64 releases."""
import contextlib, fcntl, hashlib, json, os, pathlib, re, shutil, subprocess, sys, tarfile, tempfile, urllib.request
from urllib.parse import quote, urlparse

REPO = "AmazingPVPr/Relay"
API = "https://api.github.com/repos/" + REPO + "/releases/latest"
RID = re.compile(r"^[0-9]{8}T[0-9]{6}Z-[0-9]+$")
MAX_ARCHIVE = 300 * 1024 * 1024
MAX_UNPACKED = 1024 * 1024 * 1024
TIMEOUT = 5

def fail(message):
    raise ValueError(message)

class SafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        host = urlparse(newurl)
        if host.scheme != "https" or not (host.hostname in ("github.com", "api.github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com") or (host.hostname or "").endswith(".githubusercontent.com")):
            fail("untrusted update redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)

opener = urllib.request.build_opener(SafeRedirect())
def fetch(url, limit):
    parsed = urlparse(url)
    if parsed.scheme != "https" or parsed.hostname not in ("api.github.com", "github.com"):
        fail("untrusted update URL")
    request = urllib.request.Request(url, headers={"User-Agent":"Relay-Updater/1", "Accept":"application/vnd.github+json"})
    with opener.open(request, timeout=TIMEOUT) as response:
        data = response.read(limit + 1)
    if len(data) > limit: fail("download exceeds size limit")
    return data

def current_root(root):
    marker = root / ".relay-updates" / "current"
    try:
        name = marker.read_text().strip()
        if not RID.fullmatch(name): return root
        selected = root / ".relay-updates" / "releases" / name
        if selected.is_symlink() or not (selected / "bin/relay").is_file(): return root
        return selected
    except OSError:
        return root

def installed_id(root):
    try:
        value = json.loads((root / "build-info.json").read_text()).get("release_id", "")
        return value if RID.fullmatch(value) else ""
    except (OSError, ValueError):
        return ""

def verify_signature(key, message, signature, temp):
    payload = temp / "signed.json"
    sig = temp / "signed.sig"
    payload.write_bytes(message); sig.write_bytes(signature)
    result = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(key),
                             "-rawin", "-in", str(payload), "-sigfile", str(sig)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    if result.returncode: fail("release signature invalid")

def safe_extract(archive, destination, release_id):
    total = 0
    seen = set()
    with tarfile.open(archive, "r:gz") as tar:
        for item in tar:
            path = pathlib.PurePosixPath(item.name)
            if (path.is_absolute() or not path.parts or path.parts[0] != release_id
                or any(x in ("", ".", "..") for x in path.parts)
                or item.name in seen or not (item.isdir() or item.isfile())):
                fail("unsafe archive entry")
            seen.add(item.name)
            if item.size < 0 or item.size > MAX_UNPACKED: fail("archive member too large")
            total += item.size
            if total > MAX_UNPACKED or len(seen) > 5000: fail("archive too large")
            target = destination.joinpath(*path.parts[1:])
            if item.isdir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(item) as source, target.open("xb") as output:
                    shutil.copyfileobj(source, output, 1024 * 1024)
                target.chmod(0o755 if item.mode & 0o111 else 0o644)
    if not (destination / "bin/relay").is_file(): fail("release has no executable")

def check_dependencies(root):
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(root / "third_party/libdave/lib") + ":" + str(root / "lib")
    for relative in ("bin/relay", "lib/libdave.so", "lib/relay/addons/librelay_obs.so",
                     "lib/relay/addons/librelay_soundboard.so", "lib/relay/addons/librelay_hiddenchannels.so"):
        path = root / relative
        if not path.is_file(): fail("release missing runtime component")
        result = subprocess.run(["ldd", "-r", str(path)], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=20)
        output = result.stdout.lower()
        if result.returncode or b"not found" in output or b"undefined symbol" in output:
            fail("release is incompatible with this system")

def update(root):
    state = root / ".relay-updates"
    state.mkdir(mode=0o700, exist_ok=True)
    if state.is_symlink(): fail("invalid update directory")
    releases = state / "releases"
    releases.mkdir(mode=0o700, exist_ok=True)
    if releases.is_symlink(): fail("invalid releases directory")
    with (state / "lock").open("a") as lock:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: return current_root(root)
        active = current_root(root)
        release = json.loads(fetch(API, 1024 * 1024))
        tag = release.get("tag_name", "")
        if not RID.fullmatch(tag) or release.get("draft") or release.get("prerelease"): fail("invalid release")
        base = "https://github.com/" + REPO + "/releases/download/" + quote(tag, safe="") + "/"
        names = {item.get("name") for item in release.get("assets", []) if isinstance(item, dict)}
        if not {"relay-update.json", "relay-update.sig"} <= names: fail("release missing signed update information")
        message = fetch(base + "relay-update.json", 16384)
        signature = fetch(base + "relay-update.sig", 256)
        with tempfile.TemporaryDirectory(prefix=".stage-", dir=releases) as scratch:
            temp = pathlib.Path(scratch)
            verify_signature(root / "updater/release-key.pem", message, signature, temp)
            metadata = json.loads(message)
            name = "relay-runtime-" + tag + ".tar.gz"
            if (metadata.get("release_id") != tag or metadata.get("platform") != "linux-x86_64-arch"
                or metadata.get("archive") != name or name not in names
                or not re.fullmatch(r"[0-9a-f]{64}", metadata.get("sha256", ""))):
                fail("invalid signed update metadata")
            if tag <= installed_id(active): return active
            if os.uname().machine != "x86_64": fail("unsupported architecture")
            content = fetch(base + name, MAX_ARCHIVE)
            if hashlib.sha256(content).hexdigest() != metadata["sha256"]: fail("archive digest mismatch")
            archive = temp / "download.tar.gz"
            archive.write_bytes(content)
            unpacked = temp / "release"
            unpacked.mkdir()
            safe_extract(archive, unpacked, tag)
            if installed_id(unpacked) != tag: fail("release identity mismatch")
            check_dependencies(unpacked)
            final = releases / tag
            if final.exists() or final.is_symlink(): fail("release directory already exists")
            os.rename(unpacked, final)
            marker = state / (".current-" + str(os.getpid()))
            marker.write_text(tag + "\n")
            os.replace(marker, state / "current")
            return final

def main():
    root = pathlib.Path(sys.argv[1]).resolve(strict=True)
    active = current_root(root)
    try:
        active = update(root)
    except Exception as exc:
        print("Relay update check: " + str(exc), file=sys.stderr)
    print(active)

if __name__ == "__main__": main()

