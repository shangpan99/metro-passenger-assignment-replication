"""Run the archived C++ programs on the paper's stylized metro instances."""
import argparse
import math
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parent
PAPER_CASES = [
    f"N{size:04d}_R{rep:02d}_W{window}"
    for size in (100, 200, 400, 800)
    for rep in range(1, 11)
    for window in range(1, 5)
]


def executable(version):
    filename = "run_all_scales.exe" if os.name == "nt" else "run_all_scales"
    folder = ROOT / version
    for parent in (folder / "build/Release", folder / "build", folder):
        candidate = parent / filename
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(f"Build the C++ program in {folder} first.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--method", choices=["proposed", "pb", "ds", "both", "all"], required=True)
    parser.add_argument("--case", "--cases", default="ALL", type=str.upper,
                        help="One full ID, e.g. N0100_R01_W2, or ALL (default).")
    parser.add_argument("--output", type=Path, help="New output directory, relative to your current directory or absolute.")
    parser.add_argument("--dry-run", action="store_true", help="Check the inputs and replay the initial assignment.")
    parser.add_argument("--time-limit", type=float, default=1800,
                        help="Per-method, per-instance budget in seconds (default 1800).")
    args = parser.parse_args()
    if args.case != "ALL" and args.case not in PAPER_CASES:
        parser.error("Use a full paper case ID such as N0100_R01_W2, or ALL.")
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        parser.error("--time-limit must be finite and positive.")
    if not (args.dry_run or args.output):
        parser.error("--output is required for a solver run.")
    cases = PAPER_CASES if args.case == "ALL" else [args.case]
    jobs = []
    if args.method in ("proposed", "pb", "both", "all"):
        mode = {"proposed": "proposed", "pb": "sar", "both": "both", "all": "both"}[args.method]
        jobs.append(("two_methods", mode))
    if args.method in ("ds", "all"):
        jobs.append(("three_methods", "constant"))
    try:
        programs = [(executable(version), version, mode) for version, mode in jobs]
        output = args.output.resolve() if args.output else None
        if output and not args.dry_run:
            output.mkdir(parents=True, exist_ok=False)
        for program, version, mode in programs:
            command = [str(program), "--data", str(ROOT / "data"), "--serial",
                       "--time-limit", str(args.time_limit), "--method", mode, "--cases", *cases]
            working_directory = ROOT
            if args.dry_run:
                command.append("--dry-run")
            else:
                working_directory = output
                if len(programs) > 1:
                    working_directory = output / version
                    working_directory.mkdir()
                command.extend(["--output", "."])
            result = subprocess.run(command, cwd=working_directory)
            if result.returncode:
                return result.returncode
    except FileExistsError:
        parser.exit(2, f"Output already exists. Choose a new directory: {output}\n")
    except OSError as error:
        parser.exit(1, f"{error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
