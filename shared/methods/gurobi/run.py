"""Run archived FULL or FIFO-relaxed MILPs for one instance or one demand scale."""
import argparse
from datetime import datetime
import json
import math
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parent
SCALES = (100, 200, 400, 800)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--instance", help="For example, N0100_R01_W1")
    group.add_argument("--scale", type=int, choices=SCALES, help="Run R01–R10 and W1–W4")
    parser.add_argument("--method", choices=("FULL", "LB1", "both"), default="both")
    parser.add_argument("--time-limit", type=float, default=1800, help="Seconds per method and case")
    parser.add_argument("--output", type=Path, help="A new or empty results directory")
    parser.add_argument("--dry-run", action="store_true", help="Verify archived input hashes and print the task count")
    args = parser.parse_args()
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        parser.error("--time-limit must be finite and positive")
    if args.instance:
        match = re.fullmatch(r"N(0100|0200|0400|0800)_R(0[1-9]|10)_W[1-4]", args.instance)
        if not match:
            parser.error("Expected an instance such as N0100_R01_W1")
        scale = int(match[1])
        cases = [args.instance]
    else:
        scale = args.scale
        cases = [f"N{scale:04}_R{r:02}_W{w}" for r in range(1, 11) for w in range(1, 5)]
    package = ROOT / f"N{scale}"
    sys.path.insert(0, str(package))
    from common import verify_case
    for case in cases:
        verify_case(case)
    methods = ["FULL", "LB1"] if args.method == "both" else [args.method]
    print(f"{len(cases)} verified instances, {len(cases) * len(methods)} runs", flush=True)
    if args.dry_run:
        return 0

    from run_support import monitored
    settings = json.loads((package / "experiment_settings.json").read_text(encoding="utf-8-sig"))
    output = args.output or ROOT / "runs" / datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    output = output.resolve()
    if output == ROOT or any(p in output.parents or p == output for p in
                              [ROOT / "evidence"] + [ROOT / f"N{n}" for n in SCALES]):
        parser.error("Choose a results directory outside the archived inputs and evidence")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("--output must refer to an empty or new directory")
    failed = False
    for case in cases:
        for method in methods:
            command = [sys.executable, "-B", str(package / "03_Gurobi" / "worker.py"),
                       "--method", method, "--instance", case, "--time-limit", str(args.time_limit)]
            result = monitored(command, output / case / method, args.time_limit, settings)
            print(case, method, result["status"], result.get("final_objective"), flush=True)
            if result["exit_code"] or result["status"] in ("PROCESS_ERROR", "ERROR", "WATCHDOG_TIMEOUT_NOT_CERTIFIED"):
                failed = True
                if result["status"] in ("PROCESS_ERROR", "ERROR"):
                    print(f"Worker failed. See {output / case / method / 'console.log'}", file=sys.stderr)
                    return 1
    print(f"Results: {output}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
