#!/usr/bin/env python3
"""Relay startup updater: authenticated, isolated Linux x86-64 releases."""
import argparse, contextlib, fcntl, hashlib, json, os, pathlib, re, shutil, subprocess, sys, tarfile, tempfile, time, urllib.request
from urllib.parse import quote, urlparse

REPO = "AmazingPVPr/Relay"
API = "https://api.github.com/repos/" + REPO + "/releases?per_page=100"
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

def version_key(value):
    if not isinstance(value, str) or len(value) > 64 or not RID.fullmatch(value):
        return ("", -1)
    stamp, serial = value.rsplit("-", 1)
    return (stamp, int(serial))

def installed_id(root):
    try:
        value = json.loads((root / "build-info.json").read_text()).get("release_id", "")
        return value if version_key(value)[0] else ""
    except (OSError, ValueError, AttributeError):
        return ""

def current_root(root):
    marker = root / ".relay-updates" / "current"
    try:
        name = marker.read_text().strip()
        if not version_key(name)[0]: return root
        selected = root / ".relay-updates" / "releases" / name
        if selected.is_symlink() or not (selected / "bin/relay").is_file() or installed_id(selected) != name:
            return root
        # A reinstall or local deployment may be newer than the cached update.
        return selected if version_key(name) > version_key(installed_id(root)) else root
    except OSError:
        return root

def select_release(listing):
    # Filter by platform assets before comparing versions. Windows-only releases
    # and previews cannot conceal a newer compatible, signed Linux package.
    if isinstance(listing, dict): listing = [listing]  # Older API/fixture shape.
    if not isinstance(listing, list): fail("invalid release listing")
    candidates = []
    for release in listing:
        if not isinstance(release, dict) or release.get("draft"): continue
        tag = release.get("tag_name", "")
        if not version_key(tag)[0]: continue
        names = {item.get("name") for item in release.get("assets", []) if isinstance(item, dict)}
        if {"relay-update.json", "relay-update.sig", "relay-runtime-" + tag + ".tar.gz"} <= names:
            candidates.append(release)
    return max(candidates, key=lambda item: version_key(item["tag_name"]), default=None)

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

def update(root, check_only=False):
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
        release = select_release(json.loads(fetch(API, 2 * 1024 * 1024)))
        if release is None: return active
        tag = release["tag_name"]
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
            if version_key(tag) <= version_key(installed_id(active)): return active
            if check_only: return tag
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
            if final.exists() or final.is_symlink():
                usable = False
                if not final.is_symlink() and installed_id(final) == tag and (final / "bin/relay").is_file():
                    try:
                        check_dependencies(final)
                        usable = True
                    except (OSError, ValueError):
                        pass
                if not usable:
                    # Preserve an interrupted/corrupt cache for recovery and
                    # replace it only with the freshly verified package.
                    os.rename(final, releases / (".invalid-" + tag + "-" + str(time.time_ns())))
                    os.rename(unpacked, final)
            else:
                os.rename(unpacked, final)
            marker = state / (".current-" + str(os.getpid()))
            marker.write_text(tag + "\n")
            os.replace(marker, state / "current")
            return final

def write_status(root, status):
    try:
        state = root / ".relay-updates"
        state.mkdir(mode=0o700, exist_ok=True)
        if state.is_symlink(): return
        temp = state / (".status-" + str(os.getpid()))
        temp.write_text(json.dumps(status, sort_keys=True) + "\n")
        os.replace(temp, state / "status.json")
    except OSError:
        pass

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--select", action="store_true")
    parser.add_argument("--restart-after", type=int)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    active = current_root(root)
    if args.select:
        print(active)
        return
    if args.restart_after is not None:
        if args.restart_after <= 1: fail("invalid restart process")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try: os.kill(args.restart_after, 0)
            except ProcessLookupError: break
            time.sleep(0.1)
        else: fail("Relay is still closing. Open Relay again when it has closed.")
        os.execv("/bin/bash", ["/bin/bash", str(root / "run.sh")])
    error = ""
    available = ""
    try:
        result = update(root, check_only=args.check)
        if args.check:
            available = result if isinstance(result, str) else ""
        else:
            active = result
    except Exception as exc:
        error = str(exc)
    status = {"current_version": installed_id(active), "available_version": available,
              "runtime_path": str(active), "error": error,
              "checked_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    write_status(root, status)
    if args.json:
        print(json.dumps(status))
    elif args.check:
        print(available)
    else:
        print(active)
    if error:
        print("Relay update check: " + error, file=sys.stderr)
        if args.check or args.json: raise SystemExit(2)

if __name__ == "__main__": main()
