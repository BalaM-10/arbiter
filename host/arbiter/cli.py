"""Command-line entry point: ``arbiter contract compile|check|corpus``, ``arbiter bench run``,
``arbiter evidence verify`` and ``arbiter planner run|e2e``."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .contract import ContractError, compile_file
from .corpus import write_corpus


def _contract_compile(args: argparse.Namespace) -> int:
    table = compile_file(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(table.pack())
    print(f"compiled {args.source} -> {args.output}")
    print(f"source sha256 {table.source_sha256.hex()}")
    if args.dump:
        print(table.dump(), end="")
    return 0


def _contract_check(args: argparse.Namespace) -> int:
    table = compile_file(args.source)
    print(f"{args.source}: valid")
    print(table.dump(), end="")
    return 0


def _contract_corpus(args: argparse.Namespace) -> int:
    manifest = write_corpus(compile_file(args.source), args.output)
    print(f"wrote {manifest}")
    return 0


def _bench_run(args: argparse.Namespace) -> int:
    from .bench.runner import run_bench  # imported here: it parses the C headers at import

    only = [s.strip().upper() for s in args.scenarios.split(",")] if args.scenarios else None
    return run_bench(args.target, args.contract, only, args.out, deploy=not args.no_deploy)


def _evidence_verify(args: argparse.Namespace) -> int:
    from .evidence import verify

    anchor = args.anchor
    if args.anchor_file is not None:
        anchor = args.anchor_file.read_text(encoding="ascii").strip()
    r = verify(args.log.read_text(encoding="ascii", errors="replace"), anchor=anchor)
    if not r.ok:
        for e in r.errors:
            print(f"FAIL {args.log}: {e}")
        return 1
    print(f"PASS {args.log}: {r.lines} lines, {r.records} decisions, {r.lost} declared lost, "
          f"{'ended cleanly' if r.terminated else 'unterminated (no end record)'}, "
          f"{'anchor matches' if anchor is not None else 'no anchor given'}")
    print(f"contract sha256 {r.contract}")
    print(f"last hash {r.last_hash}")
    return 0


def _evidence_verify_packet(args: argparse.Namespace) -> int:
    from . import packet

    r = packet.verify(args.dir)
    for e in r.errors:
        print(f"FAIL {args.dir}: {e}")
    if r.ok:
        print(f"PASS {args.dir}: " + "; ".join(r.checked))
    return 0 if r.ok else 1


def _planner_run(args: argparse.Namespace) -> int:
    from . import planner as pl

    plan = pl.scripted_plan()
    r = pl.run(args.host, args.port, plan, wait=not args.no_wait, log=print)
    problems = pl.check(r, plan)
    for x in problems:
        print(f"  problem: {x}")
    print(pl.summary(r, plan, problems))
    return 0 if not problems else 1


def _planner_e2e(args: argparse.Namespace) -> int:
    from .planner_e2e import fr_p1_scripted_planner_runs_pick_and_place_through_the_gateway as e2e

    return e2e(args.target, args.out, deploy=not args.no_deploy)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="arbiter", description="Arbiter host tools")
    sub = parser.add_subparsers(dest="tool", required=True)

    contract = sub.add_parser("contract", help="safety contract compiler")
    csub = contract.add_subparsers(dest="action", required=True)

    p = csub.add_parser("compile", help="validate a YAML contract and write the binary table")
    p.add_argument("source", type=Path)
    p.add_argument("-o", "--output", type=Path, required=True)
    p.add_argument("--dump", action="store_true", help="also print the table fields")
    p.set_defaults(func=_contract_compile)

    p = csub.add_parser("check", help="validate a YAML contract and print the table fields")
    p.add_argument("source", type=Path)
    p.set_defaults(func=_contract_check)

    p = csub.add_parser("corpus", help="write the cross-language test corpus for a contract")
    p.add_argument("source", type=Path)
    p.add_argument("-o", "--output", type=Path, required=True)
    p.set_defaults(func=_contract_corpus)

    bench = sub.add_parser("bench", help="fault bench against a running target")
    bsub = bench.add_subparsers(dest="action", required=True)
    p = bsub.add_parser("run", help="run scenarios on the target and write report.json and report.html")
    p.add_argument("--target", required=True, help="a target named in scripts/targets.env (qemu, pi5)")
    p.add_argument("--contract", type=Path, default=Path("contracts/default.yaml"))
    p.add_argument("--scenarios", help="comma-separated IDs, e.g. F1,F5 (default: all implemented)")
    p.add_argument("--out", type=Path, default=Path("evidence/bench"))
    p.add_argument("--no-deploy", action="store_true", help="skip building and deploying the binaries")
    p.set_defaults(func=_bench_run, source=None)

    ev = sub.add_parser("evidence", help="evidence logs written by arb-logd")
    esub = ev.add_subparsers(dest="action", required=True)
    p = esub.add_parser("verify", help="recompute the hash chain of an evidence log (docs/EVIDENCE_LOG.md)")
    p.add_argument("log", type=Path)
    p.add_argument("--anchor", help="the last hash, published separately: detects lines removed from the end")
    p.add_argument("--anchor-file", type=Path, help="read the anchor from a file (e.g. <log>.anchor)")
    p.set_defaults(func=_evidence_verify, source=None)
    p = esub.add_parser("verify-packet", help="check a bench folder's evidence packet (packet.json, FR-E3)")
    p.add_argument("dir", type=Path)
    p.set_defaults(func=_evidence_verify_packet, source=None)

    pln = sub.add_parser("planner", help="the scripted planner (docs/PROTOCOL.md)")
    psub = pln.add_subparsers(dest="action", required=True)
    p = psub.add_parser("run", help="play the pick-and-place path to a gateway")
    p.add_argument("--host", required=True, help="the gateway's address (the QNX target)")
    p.add_argument("--port", type=int, default=7700)
    p.add_argument("--no-wait", action="store_true", help="send each command as soon as the last is answered")
    p.set_defaults(func=_planner_run, source=None)
    p = psub.add_parser("e2e", help="end to end on a target: start the system there, run the planner here")
    p.add_argument("--target", required=True, help="a target in scripts/targets.env (qemu, qemu_secpol, pi5)")
    p.add_argument("--out", type=Path, default=Path("evidence/runs"))
    p.add_argument("--no-deploy", action="store_true", help="skip building and deploying the binaries")
    p.set_defaults(func=_planner_e2e, source=None)

    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except ContractError as err:
        print(f"contract invalid: {args.source}", file=sys.stderr)
        for problem in err.problems:
            print(f"  {problem}", file=sys.stderr)
        return 2
    except OSError as err:
        print(f"error: {err}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
