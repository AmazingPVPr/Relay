#!/usr/bin/env python3
"""Collect native DLL closure, licenses and an isolated launch check; create a ZIP."""
import datetime,hashlib,json,os,pathlib,re,shutil,subprocess,sys,tempfile,time
def dependencies(path):
 result=subprocess.run(["objdump","-p",str(path)],check=True,text=True,capture_output=True)
 return re.findall(r"DLL Name:\s*(\S+)",result.stdout,re.I)
def main():
 stage,deps,mingw=map(lambda p:pathlib.Path(p).resolve(),sys.argv[1:])
 source=pathlib.Path(__file__).resolve().parents[2];target=stage/"bin"
 for rel in ("platforms/qoffscreen.dll","tls/qopensslbackend.dll"):
  plugin=mingw/"share/qt6/plugins"/rel
  if plugin.exists():
   destination=stage/"plugins"/rel;destination.parent.mkdir(parents=True,exist_ok=True)
   shutil.copy2(plugin,destination)
 system=pathlib.Path(os.environ["SystemRoot"])/"System32";lookup={}
 for directory in (system,mingw/"bin",deps/"bin",target):
  for path in directory.glob("*.dll"):lookup[path.name.lower()]=path
 queue=list(stage.rglob("*.exe"))+list(stage.rglob("*.dll"));seen=set()
 while queue:
  path=queue.pop()
  if path in seen:continue
  seen.add(path)
  for name in dependencies(path):
   lower=name.lower()
   if lower.startswith(("api-ms-win-","ext-ms-win-")):continue
   origin=lookup.get(lower)
   if origin is None:raise SystemExit("Unresolved DLL: "+name+" from "+str(path))
   if origin.parent==system:continue
   destination=target/origin.name
   if not destination.exists():shutil.copy2(origin,destination)
   lookup[lower]=destination;queue.append(destination)
 notices=stage/"share/relay/licenses"
 for prefix in (mingw,deps):
  directory=prefix/"share/licenses"
  if directory.is_dir():shutil.copytree(directory,notices,dirs_exist_ok=True)
 if (deps/"share/relay/licenses").exists():shutil.copytree(deps/"share/relay/licenses",notices,dirs_exist_ok=True)
 (notices/"installed-packages.txt").write_text(subprocess.check_output(["pacman","-Q"],text=True))
 shutil.copy2(source/"LICENSE",notices/"Relay-LICENSE.txt")
 shutil.copy2(source/"docs/PORTABILITY.md",stage/"README.txt")
 for executable in (stage/"Relay.exe",target/"relay-launcher.exe"):
  if not executable.is_file():raise SystemExit("Missing Windows executable launcher: "+str(executable))
 for obsolete in ("Relay.cmd","Relay-software.cmd"):(stage/obsolete).unlink(missing_ok=True)
 release_id=os.environ.get("RELAY_RELEASE_ID",datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")+"-0")
 if not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z-[0-9]+",release_id):raise SystemExit("Invalid Windows release ID")
 (stage/"build-info.json").write_text(json.dumps({"release_id":release_id,"platform":"windows-x86_64"},indent=2)+"\n")
 # Remove developer PATH and plugin overrides: missing bundled libraries fail.
 with tempfile.TemporaryDirectory(prefix="relay-package-") as temporary:
  env=os.environ.copy()
  env.update(PATH=str(system),QT_QPA_PLATFORM="offscreen",QT_QUICK_BACKEND="software",
             XDG_CONFIG_HOME=temporary,XDG_DATA_HOME=temporary)
  for name in ("QT_PLUGIN_PATH","QML_IMPORT_PATH","QML2_IMPORT_PATH","QTDIR","QT_QPA_PLATFORM_PLUGIN_PATH",
               "QT_FFMPEG_DECODING_HW_DEVICE_TYPES","QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"):
   env.pop(name,None)
  result=subprocess.run([str(stage/"Relay.exe"),"--no-update","--demo","--quit-after","2000",
                         "--diagnostic-status",str(pathlib.Path(temporary)/"status.json")],
                        cwd=temporary,env=env,text=True,capture_output=True,timeout=30)
  if result.returncode:raise SystemExit("Packaged demo failed:\n"+result.stderr)
  status_path=pathlib.Path(temporary)/"status.json"
  deadline=time.monotonic()+25
  while not status_path.exists():
   if time.monotonic()>deadline:raise SystemExit("Windows executable launcher did not start Relay")
   time.sleep(0.1)
  status=json.loads(status_path.read_text())
  if status["authenticated"] or status["voiceConnected"]:raise SystemExit("Demo isolation failed")
 (stage/"build-validation.json").write_text(json.dumps({"platform":"windows-x86_64","release_id":release_id,"packaged_offscreen_demo":True,"packaged_launcher_demo":True,"windows_updater_test":True,
    "hardware_tested":False,"live_discord_tested":False},indent=2)+"\n")
 rows=[{"path":str(p.relative_to(stage)).replace("\\","/"),"sha256":hashlib.sha256(p.read_bytes()).hexdigest()}
       for p in sorted(stage.rglob("*")) if p.is_file() and p.name!="SHA256.json"]
 (stage/"SHA256.json").write_text(json.dumps({"files":rows},indent=2)+"\n")
 archive=shutil.make_archive(str(stage.parent/"Relay-windows-x86_64"),"zip",stage.parent,stage.name)
 pathlib.Path(archive+".sha256").write_text(hashlib.sha256(pathlib.Path(archive).read_bytes()).hexdigest()+"  "+pathlib.Path(archive).name+"\n")
 print(archive)
if __name__=="__main__":main()
