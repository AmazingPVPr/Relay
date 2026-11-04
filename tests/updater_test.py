import hashlib, importlib.util, io, json, os, pathlib, subprocess, tarfile, tempfile, unittest
SOURCE = pathlib.Path(__file__).resolve().parents[1] / "updater/relay-updater.py"
spec = importlib.util.spec_from_file_location("relay_updater", SOURCE)
updater = importlib.util.module_from_spec(spec); spec.loader.exec_module(updater)
OLD = "20261101T000000Z-1"
NEW = "20261102T000000Z-2"

class UpdateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "updater").mkdir()
        (self.root / "bin").mkdir()
        (self.root / "bin/relay").write_text("old")
        (self.root / "build-info.json").write_text(json.dumps({"release_id":OLD}))
        self.key = self.root / "secret.pem"
        subprocess.run(["openssl","genpkey","-algorithm","ED25519","-out",str(self.key)],check=True,stdout=subprocess.DEVNULL)
        subprocess.run(["openssl","pkey","-in",str(self.key),"-pubout","-out",str(self.root/"updater/release-key.pem")],check=True,stdout=subprocess.DEVNULL)
        self.old_fetch = updater.fetch
        self.old_check = updater.check_dependencies
        updater.check_dependencies = lambda root: None
    def tearDown(self):
        updater.fetch = self.old_fetch
        updater.check_dependencies = self.old_check
        self.tmp.cleanup()
    def archive(self, evil=False):
        stream=io.BytesIO()
        with tarfile.open(fileobj=stream,mode="w:gz") as tar:
            for name, data in [(NEW+"/bin/relay",b"new"), (NEW+"/build-info.json",json.dumps({"release_id":NEW}).encode())]:
                info=tarfile.TarInfo(name); info.size=len(data); info.mode=0o755
                tar.addfile(info,io.BytesIO(data))
            if evil:
                info=tarfile.TarInfo(NEW+"/../../escape"); info.size=1
                tar.addfile(info,io.BytesIO(b"x"))
        return stream.getvalue()
    def serve(self, content):
        name="relay-runtime-"+NEW+".tar.gz"
        message=json.dumps({"release_id":NEW,"platform":"linux-x86_64-arch","archive":name,"sha256":hashlib.sha256(content).hexdigest()}).encode()
        payload=self.root/"message"; signature=self.root/"signature"
        payload.write_bytes(message)
        subprocess.run(["openssl","pkeyutl","-sign","-inkey",str(self.key),"-rawin","-in",str(payload),"-out",str(signature)],check=True)
        listing=json.dumps({"tag_name":NEW,"draft":False,"prerelease":False,"assets":[{"name":x} for x in ("relay-update.json","relay-update.sig",name)]}).encode()
        data={updater.API:listing,"relay-update.json":message,"relay-update.sig":signature.read_bytes(),name:content}
        updater.fetch=lambda url,limit: data[url if url==updater.API else url.rsplit("/",1)[-1]]
    def test_newer_install_wins_over_old_cached_pointer(self):
        cached = self.root / ".relay-updates/releases" / OLD
        (cached / "bin").mkdir(parents=True)
        (cached / "bin/relay").write_text("old cached binary")
        (cached / "build-info.json").write_text(json.dumps({"release_id":OLD}))
        (self.root / ".relay-updates/current").write_text(OLD)
        (self.root / "build-info.json").write_text(json.dumps({"release_id":NEW}))
        self.assertEqual(updater.current_root(self.root), self.root)

    def test_corrupt_or_mismatched_cache_falls_back_to_bundle(self):
        cache = self.root / ".relay-updates/releases" / NEW
        (cache / "bin").mkdir(parents=True)
        (cache / "bin/relay").write_text("cached")
        (cache / "build-info.json").write_text(json.dumps({"release_id":OLD}))
        (self.root / ".relay-updates/current").write_text(NEW)
        self.assertEqual(updater.current_root(self.root),self.root)

    def test_platform_selection_skips_windows_and_accepts_signed_previews(self):
        def release(tag,names,**flags):
            return dict(tag_name=tag,assets=[{"name":name} for name in names],**flags)
        linux=release(NEW,["relay-update.json","relay-update.sig","relay-runtime-"+NEW+".tar.gz"],prerelease=True)
        windows=release("20990101T000000Z-1",["relay-update-windows.json","relay-update-windows.sig","Relay-windows-x86_64.zip"])
        draft=dict(linux,draft=True,tag_name="20990101T000000Z-3")
        self.assertEqual(updater.select_release([windows,draft,linux]),linux)
        self.assertIsNone(updater.select_release([windows,draft]))

    def test_numeric_build_serial_order(self):
        low="20261102T000000Z-99";high="20261102T000000Z-100"
        self.assertLess(updater.version_key(low),updater.version_key(high))

    def test_no_platform_release_keeps_installed_build(self):
        updater.fetch=lambda url,limit: b"[]"
        self.assertEqual(updater.update(self.root,check_only=True),self.root)

    def test_interrupted_cache_is_repaired_without_losing_the_bundle(self):
        self.serve(self.archive())
        corrupt=self.root/".relay-updates/releases"/NEW
        corrupt.mkdir(parents=True);(corrupt/"build-info.json").write_text("broken")
        chosen=updater.update(self.root)
        self.assertEqual((chosen/"bin/relay").read_text(),"new")
        self.assertTrue(any(chosen.parent.glob(".invalid-"+NEW+"-*")))
        self.assertEqual((self.root/"bin/relay").read_text(),"old")

    def test_check_only_verifies_without_downloading_or_activating(self):
        self.serve(self.archive())
        original = updater.fetch
        def fetch(url, limit):
            self.assertFalse(url.endswith(".tar.gz"))
            return original(url, limit)
        updater.fetch = fetch
        self.assertEqual(updater.update(self.root, check_only=True), NEW)
        self.assertEqual(updater.current_root(self.root), self.root)
        self.assertFalse((self.root / ".relay-updates/current").exists())

    def test_install_and_unchanged_second_run(self):
        self.serve(self.archive())
        chosen=updater.update(self.root)
        self.assertEqual((chosen/"bin/relay").read_text(),"new")
        self.assertEqual(updater.update(self.root),chosen)
        self.assertEqual(updater.current_root(self.root),chosen)
    def test_changed_archive_rejected(self):
        self.serve(self.archive())
        original=updater.fetch
        updater.fetch=lambda url,limit: b"tampered" if url.endswith(".tar.gz") else original(url,limit)
        with self.assertRaisesRegex(ValueError,"digest"): updater.update(self.root)
        self.assertEqual(updater.current_root(self.root),self.root)
    def test_unsigned_metadata_rejected(self):
        self.serve(self.archive())
        original=updater.fetch
        updater.fetch=lambda url,limit: b"wrong" if url.endswith(".sig") else original(url,limit)
        with self.assertRaisesRegex(ValueError,"signature"): updater.update(self.root)
        self.assertEqual(updater.current_root(self.root),self.root)
    def test_archive_traversal_rejected(self):
        self.serve(self.archive(evil=True))
        with self.assertRaisesRegex(ValueError,"unsafe"): updater.update(self.root)
        self.assertFalse((self.root/"escape").exists())
        self.assertEqual(updater.current_root(self.root),self.root)
    def test_incompatible_release_rejected(self):
        self.serve(self.archive())
        updater.check_dependencies = lambda root: (_ for _ in ()).throw(ValueError("incompatible"))
        with self.assertRaisesRegex(ValueError,"incompatible"): updater.update(self.root)
        self.assertEqual(updater.current_root(self.root),self.root)
    def test_offline_preserves_installed(self):
        self.serve(self.archive())
        installed=updater.update(self.root)
        updater.fetch=lambda url,limit: (_ for _ in ()).throw(OSError("offline"))
        with self.assertRaises(OSError): updater.update(self.root)
        self.assertEqual(updater.current_root(self.root),installed)

if __name__=="__main__": unittest.main()

