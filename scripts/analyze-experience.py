"""Summarize debug CSVs by process/session; no FPS filter hides slow periods.

Usage: python scripts/analyze-experience.py <experience-*.csv paths>
Optional --session N selects a session. Input ACK is injection acceptance, not
application response. Present is submission, not physical input-to-photon.
"""
import argparse
import collections
import csv
import glob
import json
import math
import pathlib


def distribution(values):
    values = sorted(values)
    if not values:
        return {"samples": 0}
    return {"samples": len(values), **{
        name: round(values[min(len(values) - 1, math.ceil(len(values) * q) - 1)], 4)
        for name, q in [("p50", .5), ("p95", .95), ("p99", .99), ("max", 1)]}}


def analyze(paths, session=None):
    groups = collections.defaultdict(list)
    frequencies, losses, invalid = {}, collections.Counter(), collections.Counter()
    for path in paths:
        path = pathlib.Path(path)
        source = str(path.resolve().parent)
        with path.open(encoding="utf-8-sig", newline="") as file:
            for raw in csv.DictReader(file):
                try:
                    row = {k: int(v) for k, v in raw.items()}
                    if any(k not in row for k in ["pid", "qpc", "event", "session", "sequence", *[f"v{i}" for i in range(10)]]):
                        raise ValueError("incomplete row")
                except (TypeError, ValueError):
                    invalid[str(path)] += 1
                    continue
                key = (source, row["pid"])
                if row["event"] == 0:
                    frequencies[key] = row["v0"]
                elif row["event"] == 6:
                    losses[key] += row["v0"]
                elif session is None or row["session"] == session:
                    groups[(*key, row["session"])].append(row)
    result = []
    for key, rows in sorted(groups.items()):
        frequency = frequencies.get(key[:2], 0)
        if not frequency:
            result.append({"source": key[0], "pid": key[1], "session": key[2], "error": "Missing QPC clock anchor"})
            continue
        rows = sorted({tuple(r.values()): r for r in rows}.values(), key=lambda r: r["qpc"])
        samples = collections.defaultdict(list)
        frames, inputs = {}, {}
        last_present, last_display = None, None
        display_skips = display_errors = 0
        for row in rows:
            event, seq = row["event"], row["sequence"]
            if event == 1:
                frames[seq] = {"arrival": row["qpc"]}
                if row["v1"] >= row["v0"] > 0:
                    samples["source_work_ms"].append((row["v1"] - row["v0"]) / 1000)
            elif event == 2:
                frame = frames.setdefault(seq, {})
                if "arrival" in frame:
                    samples["arrival_to_prepared_ms"].append((row["qpc"] - frame["arrival"]) * 1000 / frequency)
                frame["prepared"] = row["qpc"]
            elif event == 3:
                frame = frames.pop(seq, {})
                for phase in ["arrival", "prepared"]:
                    if phase in frame:
                        samples[f"{phase}_to_present_ms"].append((row["qpc"] - frame[phase]) * 1000 / frequency)
                if last_present is not None:
                    samples["present_interval_including_idle_ms"].append((row["qpc"] - last_present) * 1000 / frequency)
                last_present = row["qpc"]
                if row["v4"] != 0:
                    display_errors += 1
                    last_display = None
                elif row["v5"] > 0:
                    current = row["v5"], row["v6"]
                    if last_display and current[0] != last_display[0]:
                        delta = current[0] - last_display[0]
                        if delta == 1 and current[1] >= last_display[1]:
                            samples["observed_display_interval_vblanks"].append(current[1] - last_display[1])
                        elif delta > 1:
                            display_skips += delta - 1
                    last_display = current
            elif event == 4:
                inputs[seq] = row
            elif event == 5:
                sent = inputs.pop(seq, None)
                if sent:
                    samples["input_ack_roundtrip_ms"].append((row["qpc"] - sent["qpc"]) * 1000 / frequency)
                    if sent["v0"] >= row["v0"] > 0:
                        samples["input_capture_to_send_ms"].append((sent["v0"] - row["v0"]) / 1000)
                samples["input_host_queue_ms"].append(row["v1"] / 1000)
                samples["input_inject_ms"].append(row["v2"] / 1000)
        result.append({"source": key[0], "pid": key[1], "session": key[2],
            "duration_s": round((rows[-1]["qpc"] - rows[0]["qpc"]) / frequency, 3),
            "trace_lost_rows_for_process": losses[key[:2]],
            "dxgi_unavailable_observations": display_errors,
            "dxgi_unobserved_presents": display_skips,
            "metrics": {name: distribution(values) for name, values in samples.items()}})
    return {"sessions": result, "invalid_rows": dict(invalid), "limits": [
        "Intervals include startup, pauses, transitions and static desktops; compare annotated continuous-motion periods.",
        "Missing DXGI observations are not a measured dropped-frame count. Never substitute Present for scanout.",
        "ACK accepts injection; it does not prove application response or input-to-photon.",
        "Copy each machine into a separate directory; clocks on different PCs are not synchronized by this tool."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+")
    parser.add_argument("--session", type=int)
    args = parser.parse_args()
    paths = sorted({p for pattern in args.files for p in glob.glob(pattern)})
    if not paths:
        parser.error("No matching trace files")
    print(json.dumps(analyze(paths, args.session), ensure_ascii=False, indent=2))
