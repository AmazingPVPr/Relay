#!/usr/bin/env python3
"""Exercise real Qt audio against a private, hardware-free PipeWire/Pulse server."""
import argparse
import array
import json
import math
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import tempfile
import time
import uuid


HERE = Path(__file__).resolve().parent
RELAY = HERE.parent.parent


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, help="Build/log directory outside outputs/; default: a new /tmp directory")
    args = parser.parse_args()
    work = (args.work_dir or Path(tempfile.mkdtemp(prefix="relay-audio-backend-"))).resolve()
    if work.is_relative_to(RELAY.parent):
        raise RuntimeError("Choose a work directory outside outputs/; no generated files belong in the source deliverable")
    for program in ("cmake", "ninja", "pipewire", "pactl", "parec", "pw-link"):
        if not shutil.which(program):
            raise RuntimeError(f"Missing required program: {program}")
    work.mkdir(parents=True, exist_ok=True)
    # Unix sockets have short path limits. All runtime/state files are private
    # even when a caller reuses an existing build/log directory.
    runtime = Path(tempfile.mkdtemp(prefix="relay-audio-runtime-"))
    tag = uuid.uuid4().hex[:12]
    remote, sink = "relay-audio-" + tag, "relay_backend_test_" + tag
    socket = runtime / "pulse-native"
    config = runtime / "config"
    config.mkdir()
    configuration = (HERE / "private-pipewire.conf.in").read_text()
    configuration = configuration.replace("@REMOTE@", remote).replace("@SINK@", sink).replace("@PULSE_SOCKET@", str(socket))
    (config / "relay-audio.conf").write_text(configuration)
    environment = os.environ.copy()
    environment.update({
        "PULSE_SERVER": "unix:" + str(socket),
        "PIPEWIRE_RUNTIME_DIR": str(runtime),
        "PIPEWIRE_REMOTE": remote,
        "XDG_CONFIG_HOME": str(runtime / "user-config"),
        "XDG_STATE_HOME": str(runtime / "user-state"),
        "XDG_CACHE_HOME": str(runtime / "user-cache"),
        "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + str(runtime / "no-host-dbus"),
        "QT_AUDIO_BACKEND": "pulseaudio",
        "RELAY_AUDIO_TEST_SOCKET": str(socket),
        "RELAY_AUDIO_TEST_SINK": sink,
    })
    for name in ("user-config", "user-state", "user-cache"):
        (runtime / name).mkdir()
    environment.pop("PIPEWIRE_CONFIG_DIR", None)
    environment.pop("PIPEWIRE_CONFIG_NAME", None)
    processes = []
    server_log = None
    try:
        with (work / "build.log").open("w") as log:
            subprocess.run(["cmake", "-S", str(HERE), "-B", str(work / "build"), "-G", "Ninja"], stdout=log, stderr=subprocess.STDOUT, check=True)
            subprocess.run(["cmake", "--build", str(work / "build"), "-j2"], stdout=log, stderr=subprocess.STDOUT, check=True)
        server_log = (work / "server.log").open("w")
        server_environment = dict(environment, PIPEWIRE_CONFIG_DIR=str(config))
        server = subprocess.Popen(["pipewire", "-c", "relay-audio.conf"], env=server_environment, stdout=server_log, stderr=subprocess.STDOUT)
        processes.append(server)

        def command(arguments):
            return subprocess.run(arguments, env=environment, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=3)

        deadline = time.monotonic() + 8
        while True:
            listed = command(["pactl", "--format=json", "list", "sinks"])
            sinks = json.loads(listed.stdout or b"[]") if listed.returncode == 0 else []
            if len(sinks) == 1 and sinks[0]["name"] == sink:
                break
            if server.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("Private silent sink failed to initialize; refusing any fallback")
            time.sleep(.1)
        results = []
        for backend, muted in (("effect", False), ("effect", True), ("service", True)):
            label = backend + ("-event-muted" if muted else "-baseline")
            monitor = subprocess.Popen([
                "parec", "--client-name=RelayAudioMonitorQA", "--stream-name=RelayAudioMonitorQA",
                "--device=" + sink + ".monitor", "--format=float32le", "--rate=48000", "--channels=2", "--raw",
            ], env=environment, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            processes.append(monitor)
            probe = subprocess.Popen([str(work / "build/audio_backend_probe"), backend], env=environment,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            processes.append(probe)
            selector = selectors.DefaultSelector()
            selector.register(monitor.stdout, selectors.EVENT_READ)
            os.set_blocking(monitor.stdout.fileno(), False)
            configured, links = set(), set()
            samples, square, peak, remainder, armed = 0, 0., 0., b"", False
            role = None
            graph = {}
            began = time.monotonic()
            try:
                while probe.poll() is None:
                    if time.monotonic() - began > 13:
                        raise RuntimeError(f"{label}: backend probe timed out")
                    listed = command(["pactl", "--format=json", "list", "sink-inputs"])
                    if listed.returncode:
                        raise RuntimeError("Lost private Pulse server")
                    for entry in json.loads(listed.stdout):
                        if entry.get("properties", {}).get("application.name") != "RelayAudioBackendQA" or entry["index"] in configured:
                            continue
                        role = entry.get("properties", {}).get("media.role", "Other")
                        expected = "event" if backend == "effect" else "Other"
                        if role != expected:
                            raise RuntimeError(f"{label}: expected role {expected}, got {role}")
                        # Emulate the user's muted notification role privately.
                        # Normalize private stream gain; Relay still applies its
                        # own 50% gain to the generated sample.
                        for operation, value in (("volume", "100%"), ("mute", "1" if muted and role == "event" else "0")):
                            changed = command(["pactl", "set-sink-input-" + operation, str(entry["index"]), value])
                            if changed.returncode:
                                raise RuntimeError("Could not configure private test stream")
                        configured.add(entry["index"])
                    listed_outputs = command(["pw-link", "-o"])
                    listed_inputs = command(["pw-link", "-i"])
                    if listed_outputs.returncode or listed_inputs.returncode:
                        raise RuntimeError("Could not enumerate the private PipeWire graph")
                    outputs = listed_outputs.stdout.decode().splitlines()
                    inputs = listed_inputs.stdout.decode().splitlines()
                    graph = {"outputs": outputs, "inputs": inputs, "armed": armed, "configured_streams": len(configured),
                             "streams": [{"application": item.get("properties", {}).get("application.name"),
                                          "role": item.get("properties", {}).get("media.role", "Other")}
                                         for item in json.loads(listed.stdout)]}
                    pairs = []
                    for output in outputs:
                        if output.startswith("RelayAudioBackendQA:"):
                            pairs.extend((output, target) for target in inputs if target.startswith(sink + ":playback_")
                                         and (output.endswith("_MONO") or output[-2:] == target[-2:]))
                        if output.startswith(sink + ":monitor_"):
                            pairs.extend((output, target) for target in inputs if target.startswith("RelayAudioMonitorQA:") and output[-2:] == target[-2:])
                    # No session manager is loaded. Explicit private links are
                    # recreated when each short production cue gets a new sink.
                    linked_now = set()
                    for pair in pairs:
                        linked = command(["pw-link", "-L", *pair])
                        if linked.returncode == 0 or pair in links:
                            links.add(pair)
                            linked_now.add(pair)
                    if not armed and ((backend == "effect" and configured and len(linked_now) >= 2)
                                      or (backend == "service" and len(linked_now) >= 2)):
                        probe.stdin.write(b"P")
                        probe.stdin.flush()
                        armed = True
                    for key, _ in selector.select(.02):
                        block = remainder + (key.fileobj.read(131072) or b"")
                        count = len(block) // 4 * 4
                        remainder = block[count:]
                        values = array.array("f")
                        values.frombytes(block[:count])
                        if values:
                            peak = max(peak, max(abs(value) for value in values))
                            square += sum(value * value for value in values)
                            samples += len(values)
                    time.sleep(.03)
                output, diagnostics = probe.communicate(timeout=3)
                (work / (label + ".log")).write_bytes(diagnostics + output)
                if probe.returncode:
                    raise RuntimeError(f"{label}: probe exited {probe.returncode}")
                report = json.loads(output)
                if not report["started"] or not report["finished"] or report["error"]:
                    raise RuntimeError(f"{label}: playback failed: {report}")
                if samples < 48000 or not configured:
                    raise RuntimeError(f"{label}: insufficient real backend samples")
                if backend == "effect" and muted:
                    if peak != 0:
                        raise RuntimeError("Muted event output was not exactly silent")
                elif peak < .001:
                    raise RuntimeError(f"{label}: generated output is unexpectedly silent")
                if backend == "service":
                    counters = re.search(r"requested/started/completed/canceled/blocked/errors (\d+)/(\d+)/(\d+)/(\d+)/(\d+)/(\d+)", report["diagnostics"])
                    consumed = re.search(r"consumed PCM bytes (\d+)", report["diagnostics"])
                    if not counters or list(map(int, counters.groups())) != [4, 4, 4, 0, 0, 0] or not consumed or int(consumed[1]) <= 0:
                        raise RuntimeError("Production cues did not all complete cleanly: " + report["diagnostics"])
                result = dict(case=label, role=role, samples=samples, peak=round(peak, 6),
                              rms=round(math.sqrt(square / samples), 6), backend=report)
                results.append(result)
                print(json.dumps(result), flush=True)
            finally:
                selector.close()
                stop(probe)
                stop(monitor)
                if probe.stdout and not probe.stdout.closed:
                    leftover_output, leftover_diagnostics = probe.communicate(timeout=3)
                    if leftover_output or leftover_diagnostics:
                        with (work / (label + ".log")).open("ab") as log:
                            log.write(leftover_diagnostics + leftover_output)
                (work / (label + "-graph.json")).write_text(json.dumps(graph, indent=2) + "\n")
        (work / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print("PASS: event mute silences QSoundEffect; production PCM remains audible. Logs: " + str(work))
    finally:
        for process in reversed(processes):
            stop(process)
        if server_log:
            server_log.close()
        shutil.rmtree(runtime, ignore_errors=True)


if __name__ == "__main__":
    main()
