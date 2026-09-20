#!/usr/bin/env python3
"""
Measure the panel's microphone against its own speaker.

Why this exists. With the echo canceller disabled entirely, so that the uplink
was byte-for-byte the microphone, a real call produced this pair of results:

  - the person at the door could NOT hear the person at the panel
  - the person at the door COULD hear their own voice come back

Both travel in the same stream at the same gain. So at the panel's microphone,
the door's voice replayed through the panel's own speaker is louder than a
person standing in front of the panel and talking. If that is true, no amount
of gain fixes it -- gain lifts the echo by exactly as much as it lifts the
talker -- and no echo canceller can rescue a talker buried under its own
reference. It would be an acoustic problem with an acoustic fix.

This measures the three levels that settle it, with no call and no door:

  floor    the room, with nothing playing and nobody talking
  echo     the panel's own speaker, picked up by the panel's own microphone
  talker   a person at the panel, with the speaker silent

and reports the two differences that matter: talker over floor, and talker
over echo. The second is the one that decides the design.

  ./mic_check.py

Capture and playback run as separate processes here, deliberately: ALSA's
dmix/dsnoop let two processes share the codec in a way one process opening it
duplex cannot on this board. That difference has already cost this project an
afternoon once.
"""
import argparse
import array
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

RATE = 8000


def dbfs(samples) -> float:
    if not samples:
        return -96.0
    acc = 0.0
    for s in samples:
        acc += float(s) * float(s)
    rms = math.sqrt(acc / len(samples))
    if rms < 1.0:
        return -96.0
    return 20.0 * math.log10(rms / 32768.0)


def peak_dbfs(samples) -> float:
    if not samples:
        return -96.0
    p = max(abs(int(s)) for s in samples)
    return -96.0 if p < 1 else 20.0 * math.log10(p / 32768.0)


def write_wav(path: str, samples: array.array) -> None:
    data = samples.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)))
        f.write(data)


def read_wav(path: str) -> array.array:
    with open(path, "rb") as f:
        blob = f.read()
    i = blob.find(b"data")
    if i < 0:
        return array.array("h")
    body = blob[i + 8:]
    body = body[: len(body) - (len(body) % 2)]
    a = array.array("h")
    a.frombytes(body)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def speech_band_noise(seconds: float, level_dbfs: float) -> array.array:
    """
    Noise shaped into the telephony band, which is what the door actually sends.

    A pure tone would understate the coupling: a speaker and a microphone
    centimetres apart have a wildly uneven response, and one frequency can land
    in a null. Broadband is both more representative and harder to flatter.

    Normalised to the requested RMS after shaping, not before. The filters take
    about 7 dB out, and a signal that is 7 dB quieter than its own label would
    corrupt every comparison made against it -- which is the entire point of
    this tool.
    """
    n = int(seconds * RATE)

    seed = 22222
    lp = prev = 0.0
    hp = 0.0
    shaped = [0.0] * n
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        white = ((seed >> 8) & 0xFFFF) / 32767.5 - 1.0
        lp = 0.72 * lp + 0.28 * white           # roll off above ~1 kHz
        hp = 0.93 * (hp + lp - prev)            # and below ~150 Hz
        prev = lp
        shaped[i] = hp

    acc = 0.0
    for v in shaped:
        acc += v * v
    rms = math.sqrt(acc / n) if n else 0.0
    if rms <= 0.0:
        return array.array("h", bytes(2 * n))

    want = (10.0 ** (level_dbfs / 20.0)) * 32768.0
    scale = want / rms

    out = array.array("h", bytes(2 * n))
    clipped = 0
    for i, v in enumerate(shaped):
        x = v * scale
        if x > 32767.0:
            x = 32767.0; clipped += 1
        elif x < -32768.0:
            x = -32768.0; clipped += 1
        out[i] = int(x)

    if clipped:
        print(f"    note: {clipped} samples clipped at {level_dbfs:.0f} dBFS; "
              f"the measurement is still valid but use a lower --play-level")
    return out


def record(device: str, seconds: float, path: str) -> array.array:
    subprocess.run(
        ["arecord", "-D", device, "-f", "S16_LE", "-r", str(RATE), "-c", "1",
         "-d", str(int(math.ceil(seconds))), "-q", path],
        check=True,
    )
    return read_wav(path)


def countdown(label: str, seconds: int) -> None:
    print(f"\n  {label}")
    for s in range(3, 0, -1):
        print(f"    starting in {s}...", end="\r", flush=True)
        time.sleep(1)
    print(f"    recording for {seconds}s          ")



# ------------------------------------------------------- mixer enum helpers ---

def amixer_enum(card: str, control: str):
    """Return (items, current_item) for an ENUMERATED mixer control."""
    out = subprocess.run(["amixer", "-c", card, "cget", f"name={control}"],
                         capture_output=True, text=True).stdout
    items, current = [], None
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("; Item #"):
            q = line.split("'")
            if len(q) >= 2:
                items.append(q[1])
        elif line.startswith(": values="):
            try:
                current = items[int(line.split("=", 1)[1].split(",")[0])]
            except (ValueError, IndexError):
                pass
    return items, current


def amixer_set(card: str, control: str, value: str) -> bool:
    r = subprocess.run(["amixer", "-c", card, "cset", f"name={control}", value],
                       capture_output=True, text=True)
    return r.returncode == 0


def sweep_paths(args, far: str, cap: str) -> int:
    """
    Measure the speaker-to-microphone coupling on every playback route.

    The register diff showed that "SPK" on this board switches on the HEADPHONE
    chain -- charge pump, pre-amp, output stage -- while the dedicated SPK DAC
    stays disabled. A charge pump is a switching converter whose current draw
    tracks the output level, sharing a supply with the microphone preamp on the
    same die, which is a textbook source of exactly the coupling measured here.

    If another route drives the class-D speaker amplifier instead, it may not
    couple at all. That would make this a one-line fix rather than a hardware
    conversation, so it is worth trying every route the driver offers.
    """
    control = "Playback Path"
    items, original = amixer_enum(args.card, control)
    if not items:
        print(f"  could not read '{control}' on card {args.card}")
        return 1

    print(f"\n  '{control}' offers: {', '.join(items)}")
    print(f"  current: {original}")
    print(f"\n  Each route plays the test signal for {args.seconds}s. LISTEN to the")
    print("  panel during each one and note whether you hear anything -- a route")
    print("  that does not couple is no use if it is also silent.\n")

    results = []
    try:
        for item in items:
            if not amixer_set(args.card, control, item):
                print(f"    {item:22s} could not be selected, skipping")
                continue
            time.sleep(0.3)
            countdown(f"route '{item}' — do not talk, LISTEN", args.seconds)

            player = subprocess.Popen(["aplay", "-D", args.playback, "-q", far],
                                      stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
            time.sleep(0.4)
            got = record(args.capture, args.seconds, cap)
            player.terminate(); player.wait()

            e = dbfs(got)
            distinct = len(set(got))
            results.append((item, e, e - args.play_level, distinct))
            print(f"    echo {e:6.1f} dBFS   path {e - args.play_level:+6.1f} dB")
    finally:
        if original:
            amixer_set(args.card, control, original)
            print(f"\n  restored '{control}' = {original}")

    print("\n" + "=" * 68)
    print(f"  test signal played at {args.play_level:.1f} dBFS")
    print(f"  {'route':22s} {'echo':>9} {'path':>9}  {'distinct':>9}")
    print("-" * 68)
    for item, e, path, distinct in sorted(results, key=lambda r: r[2]):
        flag = ""
        if distinct <= 2:
            flag = "  <- ADC pinned, invalid"
        elif path < -15:
            flag = "  <- no coupling"
        print(f"  {item:22s} {e:9.1f} {path:+9.1f}  {distinct:9d}{flag}")
    print("=" * 68)
    print("\n  A route that is BOTH audible and shows no coupling is the fix.")
    print("  A route with no coupling that you could not hear is the DAC being")
    print("  off, not a solution -- the distinct-sample count catches the ADC")
    print("  pinning to a rail, which is how 'MIC OFF' fooled us once already.\n")
    return 0



# ----------------------------------------------------- linearity of the path ---

def goertzel(samples, freq: float, rate: int = RATE) -> float:
    """Amplitude at one frequency. Cheaper than a DFT and exact enough here."""
    n = len(samples)
    if n == 0:
        return 0.0
    k = round(n * freq / rate)
    w = 2 * math.pi * k / n
    cw, sw = math.cos(w), math.sin(w)
    coeff = 2 * cw
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + coeff * s1 - s2
        s2, s1 = s1, s0
    real = s1 - s2 * cw
    imag = s2 * sw
    return math.sqrt(real * real + imag * imag) * 2.0 / n


def amp_dbfs(a: float) -> float:
    return -120.0 if a < 1e-9 else 20.0 * math.log10(a / 32768.0)


def pure_tone(seconds: float, freq: float, level_dbfs: float) -> array.array:
    n = int(seconds * RATE)
    amp = (10.0 ** (level_dbfs / 20.0)) * 32768.0 * math.sqrt(2.0)
    out = array.array("h", bytes(2 * n))
    for i in range(n):
        v = amp * math.sin(2 * math.pi * freq * i / RATE)
        out[i] = int(max(-32768, min(32767, v)))
    return out


def linearity(args, cap: str) -> int:
    """
    Is the coupling linear, and therefore cancellable?

    This decides what gets built next, so it is worth measuring rather than
    assuming. A charge pump draws current in bursts tracking the MAGNITUDE of
    its output, which is a rectifying relationship -- and no linear filter,
    adaptive or otherwise, can cancel a rectified signal. If a meaningful part
    of the coupling is nonlinear, that part sets a hard floor on every
    subtraction scheme, and the honest answer becomes half duplex instead.

    Method: play a pure tone at several levels and look at what comes back.

      the fundamental tracking 1:1 in dB      -> the bulk of it is linear
      harmonics far below the fundamental     -> little rectification
      harmonics close to the fundamental      -> a linear canceller stops there

    The last figure is the one to read. The coupling has to be removed by about
    30 dB for the door to hear a talker at this panel, so harmonics sitting
    higher than -30 dB relative to the fundamental mean linear cancellation
    cannot get there however well it is implemented.
    """
    f = args.tone
    levels = [-40.0, -34.0, -28.0, -22.0, -16.0]
    tmp = os.path.dirname(cap)

    print(f"\n  tone at {f:.0f} Hz, harmonics checked at {2*f:.0f} and {3*f:.0f} Hz")
    print(f"  {len(levels)} levels, {args.seconds}s each. Do not talk during any of them.\n")
    print(f"  {'played':>8} {'fundamental':>12} {'2nd':>9} {'3rd':>9} {'worst h':>9}")
    print("  " + "-" * 52)

    played, fund, worst = [], [], []
    for lvl in levels:
        path = os.path.join(tmp, f"tone{int(-lvl)}.wav")
        write_wav(path, pure_tone(args.seconds + 2, f, lvl))

        player = subprocess.Popen(["aplay", "-D", args.playback, "-q", path],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        time.sleep(0.4)
        got = record(args.capture, args.seconds, cap)
        player.terminate(); player.wait()

        # Trim the edges: the start and stop transients are not the steady
        # state being measured.
        edge = RATE // 4
        core = got[edge:len(got) - edge] if len(got) > 2 * edge else got

        d1 = amp_dbfs(goertzel(core, f))
        d2 = amp_dbfs(goertzel(core, 2 * f))
        d3 = amp_dbfs(goertzel(core, 3 * f))
        w = max(d2, d3) - d1

        played.append(lvl); fund.append(d1); worst.append(w)
        print(f"  {lvl:8.1f} {d1:12.1f} {d2:9.1f} {d3:9.1f} {w:+9.1f}")

    n = len(played)
    mx = sum(played) / n
    my = sum(fund) / n
    den = sum((x - mx) ** 2 for x in played)
    slope = sum((x - mx) * (y - my) for x, y in zip(played, fund)) / den if den else 0.0
    worst_h = max(worst)

    print("\n" + "=" * 68)
    print(f"  slope of coupled level against played level   {slope:5.2f}   (1.00 = linear)")
    print(f"  worst harmonic, relative to the fundamental  {worst_h:+6.1f} dB")
    print("=" * 68 + "\n")

    # The distortion is second order -- the tables show the harmonic growing
    # about 12 dB for every 6 dB of drive, which is the 2:1 signature. That has
    # a consequence worth spelling out rather than leaving the reader to spot:
    # dropping the playback level by S dB improves the harmonic-to-fundamental
    # ratio by S dB AND reduces the cancellation needed by S dB. The margin
    # moves at twice the rate of the cut.
    #
    # So the verdict cannot be read off the worst level tested. Reading it that
    # way is how an earlier version of this tool called the job impossible when
    # the only thing wrong was drive level.
    usable = [(p_, f_, h_) for p_, f_, h_ in zip(played, fund, worst)
              if f_ < -2.0]                      # drop anything near the rails
    clipped = len(played) - len(usable)

    print(f"  slope of coupled level against played level   {slope:5.2f}   (1.00 = linear)")
    print(f"  worst harmonic across all levels             {worst_h:+6.1f} dB")
    if clipped:
        print(f"  ({clipped} top level(s) ignored below: the fundamental reached the rails,")
        print("   so their harmonics are the converter clipping, not the coupling)")

    if not usable:
        print("\n  Every level clipped. Re-run with a lower --play-level.\n")
        return 0

    ref_played, ref_fund, ref_h = usable[-1]
    gains = [f_ - p_ for p_, f_, _ in usable]
    gain_spread = max(gains) - min(gains)

    print(f"\n  at {ref_played:.0f} dBFS played, the highest level that did not clip:")
    print(f"    coupling gain      {ref_fund - ref_played:+6.1f} dB")
    print(f"    worst harmonic     {ref_h:+6.1f} dB below the fundamental")
    print(f"    gain varies by     {gain_spread:6.1f} dB across the levels tested")
    print("=" * 68 + "\n")

    if gain_spread > 1.5 or not (0.85 <= slope <= 1.15):
        print(f"  The gain is not constant with level (varies {gain_spread:.1f} dB, slope")
        print(f"  {slope:.2f}). That is not a linear path and a linear canceller cannot")
        print("  remove it. Half duplex.\n")
        return 0

    print(f"  Linear at a fixed level: the gain holds to {gain_spread:.1f} dB over the")
    print("  range tested, so a measured filter can subtract it. The harmonic")
    print("  content is the ceiling, and because it is second order it improves")
    print("  1 dB for every 1 dB the speaker comes down.")
    print()
    print("  Run --trade to see what speaker reduction that implies, once you")
    print("  have measured the talker level with --only talker.")
    print()
    return 0



def trade(args) -> int:
    """
    How far the speaker has to come down for a canceller to be enough.

    Everything here comes from numbers measured by the other modes, passed in
    on the command line, so this does arithmetic rather than assuming anything:

      --coupling    dB of gain from playback to microphone   (--linearity)
      --harmonic    worst harmonic below the fundamental, at --at-level
      --at-level    the playback level that harmonic was measured at
      --talker      a person at the panel                    (--only talker)
      --door-level  how loud the door's speech arrives

    The distortion is second order, so a cut of S dB in playback buys S dB of
    harmonic headroom and removes S dB of the cancellation needed. The margin
    therefore improves at 2 dB per dB, which is why a problem that looks
    hopeless at full drive is comfortable a few dB down.
    """
    print(f"\n  coupling {args.coupling:+.1f} dB, harmonic {args.harmonic:+.1f} dB "
          f"at {args.at_level:.0f} dBFS")
    print(f"  talker {args.talker:.1f} dBFS, door arrives at {args.door_level:.1f} dBFS\n")
    print(f"  {'speaker':>9} {'coupled':>9} {'needed':>8} {'possible':>9} {'margin':>8}")
    print("  " + "-" * 48)

    best = None
    for cut in range(0, 31, 2):
        played     = args.door_level - cut
        coupled    = played + args.coupling
        needed     = coupled - args.talker
        possible   = -args.harmonic + (args.at_level - played)
        margin     = possible - needed
        mark = ""
        if margin >= 6.0 and best is None:
            best = cut
            mark = "  <- first comfortable setting"
        print(f"  {-cut:8d}dB {coupled:9.1f} {needed:8.1f} {possible:9.1f} "
              f"{margin:+8.1f}{mark}")

    print()
    if best is None:
        print("  No cut in this range gives 6 dB of margin. Half duplex.\n")
    elif best == 0:
        print("  It already has margin at full level. Build the canceller and\n"
              "  leave the speaker alone.\n")
    else:
        print(f"  speaker_gain_db = -{best}  gives a canceller enough room to work.")
        print(f"  That is {best} dB quieter at the panel, which is the cost, and it")
        print("  is the only part of this a config file can change.\n")
        print("  A cut alone does NOT fix it -- the coupling still sits well above")
        print("  the talker. It is what makes the canceller sufficient.\n")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", default="default")
    ap.add_argument("--playback", default="default")
    ap.add_argument("--seconds", type=int, default=6)
    ap.add_argument("--play-level", type=float, default=-20.0,
                    help="dBFS the test signal is played at; -20 is about what "
                         "the door's own speech arrives as (default: -20)")
    ap.add_argument("--only", choices=["floor", "echo", "talker"],
                    help="run a single phase. Use --only echo to repeat the "
                         "coupling measurement under different conditions "
                         "(speaker muffled, speaker path off) without sitting "
                         "through the other two.")
    ap.add_argument("--card", default="0",
                    help="ALSA card index for amixer (default: 0)")
    ap.add_argument("--linearity", action="store_true",
                    help="play a tone at several levels and measure whether "
                         "the coupling is linear, which decides whether it can "
                         "be cancelled at all")
    ap.add_argument("--tone", type=float, default=400.0,
                    help="tone frequency for --linearity (default: 400)")
    ap.add_argument("--trade", action="store_true",
                    help="work out what speaker reduction makes a canceller "
                         "sufficient, from figures the other modes measured")
    ap.add_argument("--coupling",   type=float, default=15.3)
    ap.add_argument("--harmonic",   type=float, default=-33.5)
    ap.add_argument("--at-level",   type=float, default=-22.0)
    ap.add_argument("--talker",     type=float, default=-39.0)
    ap.add_argument("--door-level", type=float, default=-20.0)
    ap.add_argument("--sweep-paths", action="store_true",
                    help="measure the coupling on every Playback Path the "
                         "driver offers, then restore the original")
    ap.add_argument("--label", default="",
                    help="printed with the result, so a series of --only echo "
                         "runs can be told apart afterwards")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(prefix="syncn-mic-")
    far = os.path.join(tmp, "far.wav")
    cap = os.path.join(tmp, "cap.wav")

    write_wav(far, speech_band_noise(args.seconds + 2, args.play_level))

    print(__doc__.split("\n\n")[0])
    print(f"\ncapture={args.capture}  playback={args.playback}  "
          f"{args.seconds}s per phase  test signal at {args.play_level:.0f} dBFS")

    if args.trade:
        return trade(args)

    if args.linearity:
        return linearity(args, cap)

    if args.sweep_paths:
        return sweep_paths(args, far, cap)

    # ---- a single phase, for repeating one measurement -------------------
    if args.only == "echo":
        tag = f" [{args.label}]" if args.label else ""
        countdown("The panel will PLAY. Do not talk.", args.seconds)
        player = subprocess.Popen(["aplay", "-D", args.playback, "-q", far],
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        time.sleep(0.4)
        got = record(args.capture, args.seconds, cap)
        player.terminate(); player.wait()
        e = dbfs(got)
        print(f"\n    played  {args.play_level:6.1f} dBFS")
        print(f"    echo    {e:6.1f} dBFS   (peak {peak_dbfs(got):.1f})")
        print(f"    path    {e - args.play_level:+6.1f} dB{tag}")
        print("\n  A path with GAIN is not air. A speaker and a microphone in one")
        print("  case lose 15 to 35 dB between them. If muffling the speaker with")
        print("  a hand barely changes this number, the signal is not crossing the")
        print("  room at all -- it is looping back inside the codec, and that is a")
        print("  register or a routing switch rather than an acoustic problem.\n")
        return 0

    if args.only in ("floor", "talker"):
        what = ("silence. Do not talk. Nothing will play."
                if args.only == "floor"
                else "TALK NORMALLY at the panel for the whole recording.")
        countdown(what, args.seconds)
        got = record(args.capture, args.seconds, cap)
        tag = f" [{args.label}]" if args.label else ""
        print(f"\n    {args.only:7s} {dbfs(got):6.1f} dBFS   "
              f"(peak {peak_dbfs(got):.1f}){tag}\n")
        return 0

    # ---- 1. the room -----------------------------------------------------
    countdown("PHASE 1 of 3 — silence. Do not talk. Nothing will play.",
              args.seconds)
    floor = dbfs(record(args.capture, args.seconds, cap))
    print(f"    floor   {floor:6.1f} dBFS")

    # ---- 2. our own speaker into our own microphone ----------------------
    countdown("PHASE 2 of 3 — the panel will PLAY. Still do not talk.",
              args.seconds)
    player = subprocess.Popen(["aplay", "-D", args.playback, "-q", far],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    time.sleep(0.4)                       # let the buffer fill
    echo_samples = record(args.capture, args.seconds, cap)
    player.terminate()
    player.wait()
    echo = dbfs(echo_samples)
    echo_pk = peak_dbfs(echo_samples)
    print(f"    echo    {echo:6.1f} dBFS   (peak {echo_pk:.1f})")

    # ---- 3. a person -----------------------------------------------------
    countdown("PHASE 3 of 3 — TALK NORMALLY at the panel, from where a visitor "
              "would stand.\n    Keep talking for the whole recording.",
              args.seconds)
    talk_samples = record(args.capture, args.seconds, cap)
    talker = dbfs(talk_samples)
    talk_pk = peak_dbfs(talk_samples)
    print(f"    talker  {talker:6.1f} dBFS   (peak {talk_pk:.1f})")

    # ---- what it means ---------------------------------------------------
    print("\n" + "=" * 68)
    print(f"  floor    {floor:7.1f} dBFS")
    print(f"  echo     {echo:7.1f} dBFS      speaker into mic")
    print(f"  talker   {talker:7.1f} dBFS      a person at the panel")
    print("-" * 68)
    print(f"  talker over floor   {talker - floor:+6.1f} dB")
    print(f"  talker over echo    {talker - echo:+6.1f} dB   <-- the one that decides")
    print(f"  speaker to mic      {echo - args.play_level:+6.1f} dB   <-- is this even acoustic?")
    print("=" * 68)

    margin = talker - echo
    path   = echo - args.play_level
    print()

    if path > -10.0:
        print(f"  FIRST: that path shows {path:+.1f} dB. A speaker and a microphone in")
        print("  one case lose 15 to 35 dB between them -- they do not gain. Before")
        print("  treating any of this as acoustic, re-run phase 2 with the speaker")
        print("  muffled under a hand:")
        print()
        print("      mic_check.py --only echo --label muffled")
        print()
        print("  Barely any change means the signal never crossed the room: it is")
        print("  looping back inside the codec, which is a register or a routing")
        print("  switch, not a gasket. Everything below assumes it really is air.")
        print()
    if margin >= 10:
        print("  The talker is well clear of the echo. The acoustics are fine and")
        print("  the fault is elsewhere -- gain, or the door. Raise mic_gain_db")
        print(f"  by about {int(-18 - talker)} dB to bring the talker to -18 dBFS.")
    elif margin >= 0:
        print("  The talker is only just above its own echo. A canceller can work")
        print("  here but has little margin, and gain alone will not help: it")
        print("  lifts both by the same amount. Lower speaker_gain_db first, then")
        print("  raise mic_gain_db, and re-run this.")
    else:
        print("  The panel hears its own speaker LOUDER than it hears a person.")
        print("  This is an acoustic problem and it has no software fix: gain")
        print("  lifts the echo by exactly as much as the talker, and no echo")
        print(f"  canceller can recover a talker sitting {-margin:.0f} dB under its own")
        print("  reference. What changes it:")
        print("    - turn the panel's speaker down (speaker_gain_db, or the ALSA")
        print("      mixer), which costs nothing and moves the ratio directly")
        print("    - check the microphone port is not blocked, and is not on the")
        print("      same face of the case as the speaker with no gasket between")
        print("    - check the ALSA capture gain: a codec input at minimum looks")
        print("      exactly like this")

    print(f"\n  ALSA levels are worth a look either way:  amixer -c0 contents\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
