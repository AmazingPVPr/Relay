#!/usr/bin/env python3
"""Build pinned DAVE/MLS++ dependencies on Linux or Windows."""
import argparse,json,pathlib,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[1]
PINS=[
 ("json","https://github.com/nlohmann/json.git","55f93686c01528224f448c19128836e7df245f72"),
 ("mlspp","https://github.com/cisco/mlspp.git","1cc50a124a3bc4e143a787ec934280dc70c1034d"),
 ("libdave","https://github.com/discord/libdave.git","8de72b1f8a2ac3c5a5270755bb8091a62e3c6169")]
def run(*args):subprocess.run([str(a) for a in args],check=True)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument("--work",type=pathlib.Path,required=True)
 p.add_argument("--prefix",type=pathlib.Path,required=True)
 p.add_argument("--jobs",type=int,default=4)
 p.add_argument("--cmake-arg",action="append",default=[])
 a=p.parse_args()
 if any((parent/".relay-runtime-only").exists() for parent in (ROOT,ROOT.parent.parent)):
  p.error("This is a runtime-only mirror; build from the authoritative source.")
 if not 1<=a.jobs<=6:p.error("--jobs must be between 1 and 6")
 work=a.work.resolve();prefix=a.prefix.resolve();work.mkdir(parents=True,exist_ok=True)
 for name,url,revision in PINS:
  source=work/name
  if not (source/".git").exists():
   run("git","init",source);run("git","-C",source,"remote","add","origin",url)
  run("git","-C",source,"fetch","--depth","1","origin",revision)
  run("git","-C",source,"checkout","--detach",revision)
  src=source/"cpp" if name=="libdave" else source
  extra={"json":["-DJSON_BuildTests=OFF"],
         "mlspp":["-DMLS_CXX_NAMESPACE=mlspp","-DDISABLE_GREASE=ON","-DTESTING=OFF","-DBUILD_SHARED_LIBS=OFF"],
         "libdave":["-DBUILD_SHARED_LIBS=ON","-DTESTING=OFF","-DPERSISTENT_KEYS=OFF"]}[name]
  build=work/(name+"-build")
  run("cmake","-S",src,"-B",build,"-G","Ninja","-DCMAKE_BUILD_TYPE=Release",
      "-DCMAKE_INSTALL_PREFIX="+str(prefix),"-DCMAKE_PREFIX_PATH="+str(prefix),
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON","-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON",
      *a.cmake_arg,*extra)
  run("cmake","--build",build,"--parallel",a.jobs)
  run("cmake","--install",build)
 # Direct messaging is compiled with the application; fetch all pinned
 # submodules here so platform builders never need a Vault-only path.
 rtc=work/"libdatachannel"
 if not (rtc/".git").exists():
  run("git","init",rtc);run("git","-C",rtc,"remote","add","origin","https://github.com/paullouisageneau/libdatachannel.git")
 run("git","-C",rtc,"fetch","--depth","1","origin","6b1e2e620f1e37f0eafeee702eaea0043cb305fd")
 run("git","-C",rtc,"checkout","--detach","6b1e2e620f1e37f0eafeee702eaea0043cb305fd")
 run("git","-C",rtc,"submodule","update","--init","--recursive","--depth","1")
 licenses=prefix/"share/relay/licenses";licenses.mkdir(parents=True,exist_ok=True)
 (licenses/"libdatachannel.txt").write_bytes((rtc/"LICENSE").read_bytes())

 for name,_,_ in PINS:
  license=work/name/("LICENSE.MIT" if name=="json" else "LICENSE")
  (licenses/(name+".txt")).write_bytes(license.read_bytes())
 (licenses/"dave-sources.json").write_text(json.dumps(PINS,indent=2)+"\n")
if __name__=="__main__":main()
