#!/usr/bin/env python3
"""Golden regression runs: scripted, deterministic runs of a runner binary over every disc you have.

For each disc: run `<runner> <disc> 1 FRAMES --hand-scan --trace-api` with PPM snapshots every
SNAP frames, a WAV capture and a final RAM dump, then hash the event trace (paths stripped), each snapshot, the WAV
and the RAM. Hashes of copyrighted output only; no game data is stored.

The runs use extracted program directories under ION_EXTRACT; with --images they boot the disc images under ION_DUMPS
instead. The hashes must match.

usage: golden.py --runner PATH [--record | --compare] [--images] [--frames N] [--only disc,disc] [--jobs N]
       golden files: ioncore/tests/golden/<disc>.json
"""
import argparse, glob, hashlib, json, os, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
EXTRACT = os.environ.get('ION_EXTRACT', os.path.join(ROOT, 'extract'))
GOLDEN = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'golden')
DUMPS = os.environ.get('ION_DUMPS', os.path.join(ROOT, 'dumps'))   # your disc images (never in the repo); any subfolder
IMAGES = {                        # raw dumps usable directly, found by title (--images skips the ones you do not have)
    'blues': "*Blue's Room*.cue",
    'dora': '*Dora the Explorer*.cue',
    'batman': '*Batman*.cue',
    'spongebob': '*SpongeBob*.mdx',
    'bestofnick': '*Best of Nickelodeon*.cue',
    'dannyphantom': '*Danny Phantom*Loose.bin',   # the damaged single-file dump, unrepaired: must still match
}
def image_path(disc):
    hits = sorted(glob.glob(os.path.join(DUMPS, '**', IMAGES[disc]), recursive=True))
    return hits[0] if hits else None
SCENARIOS = {                     # disc -> extra runner args (button presses exercise Stop/Next/Open paths)
    'blues': ['--press', '0x08@15000+10'],
    'dora': [],
    'batman': ['--press', '0x80@21000+10'],
    'spongebob': [],
    'dannyphantom': [],
    'bestofnick': [],
}

def sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()

def run(runner, disc, frames, snap, images=False):
    with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as tmp:
        wav, ram = os.path.join(tmp, 'a.wav'), os.path.join(tmp, 'ram.bin')
        at = ','.join(str(f) for f in range(snap, frames, snap))
        src = image_path(disc) if images else os.path.join(EXTRACT, disc)
        cmd = [runner, src, '1', str(frames), '--hand-scan', '--trace-api', '--png-dir', tmp,
               '--png-at', at, '--wav', wav, '--dump-ram', ram] + SCENARIOS.get(disc, [])
        p = subprocess.run(cmd, capture_output=True, text=True)
        events = [re.sub(r'snapshot .*/(frame_\d+\.ppm)', r'snapshot \1', l) for l in p.stderr.splitlines()
                  if l.startswith('[f') and not l.startswith('[f0] disc:')]   # the bad-sector warning only appears on raw images
        frames_h = {f: sha1(os.path.join(tmp, f)) for f in sorted(os.listdir(tmp)) if f.endswith('.ppm')}
        return {'args': cmd[2:5] + SCENARIOS.get(disc, []), 'exit': p.returncode,
                'events': hashlib.sha1('\n'.join(events).encode()).hexdigest(), 'n_events': len(events),
                'frames': frames_h, 'wav': sha1(wav), 'ram': sha1(ram)}, events

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runner', required=True)
    g = ap.add_mutually_exclusive_group(); g.add_argument('--record', action='store_true'); g.add_argument('--compare', action='store_true')
    ap.add_argument('--frames', type=int, default=20000); ap.add_argument('--snap', type=int, default=2500)
    ap.add_argument('--only'); ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--images', action='store_true', help='boot the raw dumps instead of the extracted directories')
    a = ap.parse_args()
    discs = [d for d in SCENARIOS if (a.images or os.path.isdir(os.path.join(EXTRACT, d))) and (not a.only or d in a.only.split(','))]
    if a.images:
        discs = [d for d in discs if d in IMAGES and image_path(d)]
    if not discs:
        print(f'no discs to run: put your dumps under {DUMPS} (or set ION_DUMPS) and use --images, or set ION_EXTRACT to your extracted discs', file=sys.stderr)
        return 1
        if a.record: sys.exit('--images is for --compare only (the golden files come from the extracted directories)')
    os.makedirs(GOLDEN, exist_ok=True)
    with ThreadPoolExecutor(a.jobs) as ex:
        results = dict(zip(discs, ex.map(lambda d: run(os.path.abspath(a.runner), d, a.frames, a.snap, a.images), discs)))
    bad = 0
    for d, (res, events) in results.items():
        path = os.path.join(GOLDEN, d + '.json')
        if a.record:
            json.dump(res, open(path, 'w'), indent=1); print(f'{d}: recorded ({res["n_events"]} events, {len(res["frames"])} frames)')
            continue
        ref = json.load(open(path))
        diffs = [k for k in ('exit', 'events', 'n_events', 'wav', 'ram') if ref[k] != res[k]]
        diffs += [f'frame {f}' for f in sorted(set(ref['frames']) | set(res['frames'])) if ref['frames'].get(f) != res['frames'].get(f)]
        print(f'{d}: ' + ('OK' if not diffs else 'DIFF ' + ', '.join(diffs[:8])))
        bad += bool(diffs)
    sys.exit(1 if bad else 0)

if __name__ == '__main__':
    sys.exit(main() or 0)
