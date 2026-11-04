"""Regenerate Relay's original, softly damped activity cues (never plays audio)."""
from pathlib import Path
import math
import random
import struct
import wave

SAMPLE_RATE = 48000
# Each family has its own register, rhythm and resonator. Opposite actions
# retain a rising/falling relationship without sharing the same two-note cue.
# frequencies, onset times, tail, peak, brightness, resonator
CUES = {
    "mute":         ([245, 164],       [0, .065],       .130, .40, .25, "wood"),
    "unmute":       ([196, 330, 440],  [0, .045, .090], .130, .40, .25, "wood"),
    "deafen":       ([147, 147, 110],  [0, .050, .140], .145, .42, .30, "knock"),
    "undeafen":     ([110, 220, 294],  [0, .090, .150], .145, .42, .30, "knock"),
    "join":         ([330, 440, 659],  [0, .070, .140], .190, .44, .65, "chime"),
    "leave":        ([554, 370],       [0, .130],       .190, .42, .55, "chime"),
    "user-join":    ([740, 988],       [0, .055],       .145, .37, .45, "bubble"),
    "user-leave":   ([880, 587],       [0, .090],       .145, .36, .40, "bubble"),
    "stream-start": ([392, 588, 784],  [0, .040, .130], .195, .42, .80, "glass"),
    "stream-stop":  ([784, 392, 196],  [0, .085, .125], .180, .39, .65, "glass"),
    "watch-start":  ([523, 698],       [0, .115],       .170, .37, .55, "soft"),
    "watch-stop":   ([698, 523, 349],  [0, .045, .110], .165, .36, .45, "soft"),
    "message":      ([1175],          [0],             .145, .30, .65, "bell"),
}


def smooth(value):
    value = max(0.0, min(1.0, value))
    return value * value * (3.0 - 2.0 * value)


def render(name, frequencies, onsets, tail, peak, brightness, resonator):
    length = int((onsets[-1] + tail) * SAMPLE_RATE)
    result = [0.0] * length
    random_source = random.Random("relay-distinct-cue-v3:" + name)
    for note, frequency in enumerate(frequencies):
        start = round(onsets[note] * SAMPLE_RATE)
        velocity = .72 + .28 * note / max(1, len(frequencies) - 1)
        noise = 0.0
        for index in range(int(tail * SAMPLE_RATE)):
            at = start + index
            if at >= length:
                break
            t = index / SAMPLE_RATE
            # A rounded pluck with a tiny initial pitch relaxation. Partial and
            # transient energy decay quickly; there is no sustained alarm tone.
            phase = 2 * math.pi * frequency * (t + .035 * .012 * (1 - math.exp(-t / .012)))
            body = math.sin(phase + .22 * brightness * math.exp(-t / .018) * math.sin(2 * phase))
            body += .10 * brightness * math.exp(-t / .035) * math.sin(2 * phase)
            body += .045 * brightness * math.exp(-t / .022) * math.sin(3.02 * phase)
            body += .065 * math.sin(.5 * phase)
            # Distinct acoustic signatures at moderate partial levels.
            if resonator == "wood":
                body = .85 * body + .24 * math.sin(2.76 * phase) * math.exp(-t / .018)
            elif resonator == "knock":
                body = .65 * body + .40 * math.sin(1.48 * phase) * math.exp(-t / .030)
            elif resonator == "chime":
                body += .24 * math.sin(2 * phase) * math.exp(-t / .075)
            elif resonator == "bubble":
                body = math.sin(phase + .55 * math.exp(-t / .020) * math.sin(phase))
            elif resonator == "glass":
                body += .28 * math.sin(2.41 * phase) * math.exp(-t / .055)
            elif resonator == "soft":
                body = .85 * math.sin(phase) + .28 * math.sin(.5 * phase)
            elif resonator == "bell":
                body += .22 * math.sin(2.71 * phase) * math.exp(-t / .045)
            noise = .82 * noise + .18 * random_source.uniform(-1, 1)
            body += .07 * noise * math.exp(-t / .006)
            envelope = smooth(t / .004) * (.88 * math.exp(-t / .038) + .12 * math.exp(-t / .095))
            envelope *= smooth((tail - t) / .025)
            result[at] += velocity * envelope * body
    # Gentle low-pass finishing removes sharp high-frequency edges. The last
    # fade also removes the filter's tail cleanly at the resource boundary.
    filtered, previous = [], 0.0
    for value in result:
        previous += .42 * (value - previous)
        filtered.append(previous)
    dc = sum(filtered) / len(filtered)
    for index, value in enumerate(filtered):
        t = index / SAMPLE_RATE
        filtered[index] = (value - dc) * smooth(t / .003) * smooth((len(filtered) - 1 - index) / (SAMPLE_RATE * .020))
    scale = peak / max(abs(value) for value in filtered)
    pcm = [round(value * scale * 32767) for value in filtered]
    pcm[0] = pcm[-1] = 0
    return pcm


def generate():
    for name, spec in CUES.items():
        samples = render(name, *spec)
        with wave.open(str(Path(__file__).parent / (name + ".wav")), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(SAMPLE_RATE)
            output.writeframes(struct.pack("<" + "h" * len(samples), *samples))


if __name__ == "__main__":
    generate()
