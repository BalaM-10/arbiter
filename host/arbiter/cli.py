"""Command-line entry point: ``arbiter contract compile|check|corpus``, ``arbiter bench run`` and
``arbiter evidence verify``."""

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
