"""report.json and report.html for one bench run (FR-B1, FR-B2)."""

from __future__ import annotations

import html
import json
from pathlib import Path
from typing import Any

from . import kinematics as kin
from .records import Run
from .spec import Spec


def tip_paths(run: Run, every: int = 25) -> dict[str, list[list[float]]]:
    """Downsampled elbow and tip paths in mm, for the report's top view (every 25 ms: a drawing;
    the checks use every sample)."""
    out: dict[str, list[list[float]]] = {"elbow": [], "tip": []}
    for s in run.samples[::every]:
        e, t = kin.elbow_tip(s.pos[0], s.pos[1])
        out["elbow"].append([round(e[0], 1), round(e[1], 1)])
        out["tip"].append([round(t[0], 1), round(t[1], 1)])
    return out


def _svg(sc: dict[str, Any], spec: Spec) -> str:
    size, reach = 260, 600.0
    def xy(p: list[float]) -> str:
        return f"{size / 2 + p[0] / reach * size / 2:.1f},{size / 2 - p[1] / reach * size / 2:.1f}"
    parts = [f'<svg viewBox="0 0 {size} {size}" width="{size}" height="{size}" class="topview">',
             f'<circle cx="{size / 2}" cy="{size / 2}" r="3" class="base"/>']
    for z in spec.zones:
        x0, y1 = xy([z.x_min, z.y_max]).split(",")
        w = (z.x_max - z.x_min) / reach * size / 2
        h = (z.y_max - z.y_min) / reach * size / 2
        parts.append(f'<rect x="{x0}" y="{y1}" width="{w:.1f}" height="{h:.1f}" class="zone"/>')
    for name in ("elbow", "tip"):
        pts = " ".join(xy(p) for p in sc["paths"][name])
        if pts:
            parts.append(f'<polyline points="{pts}" class="{name}"/>')
    parts.append("</svg>")
    return "".join(parts)


CSS = """
:root{--bg:#fff;--fg:#1a1a1a;--muted:#666;--ok:#11753a;--bad:#b3261e;--line:#ddd;--zone:#f2b8b5}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--fg:#eee;--muted:#aaa;--ok:#6dd58c;--bad:#ff8a80;
--line:#333;--zone:#5c2b29}}
body{background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;margin:0 auto;max-width:960px;
padding:16px}
h1{font-size:20px}h2{font-size:16px;margin-top:28px}.muted{color:var(--muted)}
table{border-collapse:collapse;width:100%;margin:8px 0}td,th{border-bottom:1px solid var(--line);
padding:4px 6px;text-align:left;vertical-align:top}.pass{color:var(--ok);font-weight:600}
.fail{color:var(--bad);font-weight:600}.row{display:flex;gap:16px;flex-wrap:wrap}
.topview{border:1px solid var(--line)}.zone{fill:var(--zone)}.base{fill:var(--fg)}
.elbow{fill:none;stroke:var(--muted);stroke-width:1.5}.tip{fill:none;stroke:var(--fg);stroke-width:1.5}
details{margin:6px 0}code{font-size:12px}.checks{flex:1;min-width:280px}
"""


def _status(ok: bool) -> str:
    return f'<span class="{"pass" if ok else "fail"}">{"PASS" if ok else "FAIL"}</span>'


def _scenario_html(sc: dict[str, Any], spec: Spec) -> str:
    e = html.escape
    checks = "".join(
        f'<tr><td>{_status(c["passed"])}</td><td><code>{e(c["name"])}</code></td><td>{e(c["detail"])}</td></tr>'
        for c in sc["checks"])
    verdicts: list[dict[str, Any]] = next(
        (c["data"].get("verdicts", []) for c in sc["checks"] if c["name"] == "verdicts_as_expected"), [])
    head = "".join(f"<th>{h}</th>" for h in
                   ("t ms", "seq", "tag", "target µrad", "asked ms", "verdict/rule", "forwarded ms", "state"))
    vrows = "".join(
        "<tr>" + "".join(f"<td>{e(str(x))}</td>" for x in (
            v["t_ms"], v["seq"], v["tag"], v["target_urad"], v["requested_ms"],
            f'{v["verdict"]}/{v["rule"]}', v["forwarded_ms"], v["state"])) + "</tr>"
        for v in verdicts)
    return (
        f'<h2>{e(sc["id"])} {e(sc["title"])}: {_status(sc["passed"])}</h2>'
        f'<p><b>Pass condition.</b> {e(sc["pass_condition"])}</p>'
        f'<p><b>Commands sent by:</b> {e(sc.get("transport", "arb-inject"))}. '
        f"Judged from the arm's own telemetry, read on the target.</p>"
        f'<div class="row">{_svg(sc, spec)}<div class="checks"><table>{checks}</table></div></div>'
        f"<details><summary>{len(verdicts)} commands and the guardian's verdicts "
        f"(the safety checks above use telemetry, not these)</summary>"
        f"<table><tr>{head}</tr>{vrows}</table></details>")


def write(report: dict[str, Any], spec: Spec, out: Path) -> None:
    # Compact: the report is a view of raw/, which holds the records themselves.
    (out / "report.json").write_text(json.dumps(report, separators=(",", ":")) + "\n", encoding="utf-8")
    e = html.escape
    m = report["meta"]
    meta = (f'{e(m["started"])} · target {e(m["target"])} (<code>{e(m["uname"])}</code>) · '
            f'commit <code>{e(m["commit"])}</code> · contract {e(m["contract"])} '
            f'sha256 <code>{e(m["contract_sha256"][:16])}…</code>')
    note = ("Safety checks are computed on the host from the arm's telemetry with independent kinematics, "
            "not from the guardian's verdicts. On QEMU the bench proves function, never timing. "
            "Top views: base at the centre, operator's hand area shaded, tip path dark, elbow path grey.")
    if not m.get("tick_spacing_judged", True):
        note += (" Timing policy for this run: " + str(m["timing_policy"]) + ". "
                 "The QEMU-only change was made after a QEMU run failed on 2 late ticks "
                 "(evidence/bench/2026-11-13-qemu/153541, kept); "
                 "the same F1 check stays strict on the Pi 5.")
    body = "".join(_scenario_html(sc, spec) for sc in report["scenarios"])
    page = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            f"<title>Arbiter bench report</title><style>{CSS}</style></head><body>"
            f'<h1>Arbiter fault bench: {report["passed"]}/{report["total"]} scenarios pass</h1>'
            f'<p class="muted">{meta}</p><p class="muted">{e(note)}</p>{body}</body></html>\n')
    (out / "report.html").write_text(page, encoding="utf-8")
