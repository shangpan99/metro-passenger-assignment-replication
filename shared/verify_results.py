"""Rebuild the five-method comparison from archived values. Python 3.8+, standard library."""

import argparse
import csv
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path


ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
METHODS = ROOT / "methods"
DS = "Constant_PMC_DiminishingStep_"
STATUS = {"OPTIMAL": "OPT", "INTERRUPTED": "INT", "TIME_LIMIT": "TL", "PROCESS_ERROR": "ERR"}


def read_rows(path):
    with path.open(encoding="utf-8-sig", newline="") as handle:
        rows = list(csv.DictReader(handle))
    index = {row["instance_id"]: row for row in rows}
    if len(index) != len(rows):
        raise ValueError(f"Duplicate instance ID in {path}")
    return index


def same_number(observed, published, places=None):
    if not observed or not published:
        return observed == published
    value = Decimal(observed)
    if places is not None:
        value = value.quantize(Decimal(1).scaleb(-places), rounding=ROUND_HALF_UP)
    return value == Decimal(published)


def compare():
    paper = read_rows(RESULTS / "paper_tables.csv")
    heuristics = read_rows(METHODS / "proposed_pb" / "cpp_results.csv")
    ds = read_rows(METHODS / "ds_pmc" / "archived_results" / "comparison_instances.csv")
    gurobi = read_rows(METHODS / "gurobi" / "historical_results.csv")
    ids = [f"N{n:04d}_R{r:02d}_W{w}" for n in (100, 200, 400, 800)
           for r in range(1, 11) for w in range(1, 5)]
    if set(ids) != set(paper):
        raise ValueError("The reference must contain the four paper windows for every replicate.")
    outcomes = []
    for key in ids:
        ref, hp, step, solver = paper[key], heuristics[key], ds[key], gurobi[key]
        raw = {
            "lower_bound": solver["lower_bound"], "full_objective": solver["full_objective"],
            "full_seconds": solver["full_seconds"], "ds_objective": step[DS + "final_objective"],
            "ds_seconds": step[DS + "complete_wall_time_seconds"],
            "pb_objective": hp["pb_objective"], "pb_seconds": hp["pb_seconds"],
            "proposed_objective": hp["proposed_objective"], "proposed_seconds": hp["proposed_seconds"],
        }
        row = {"instance_id": key, **raw, "ds_best_objective": step[DS + "best_objective"],
               "full_batch_status": solver["full_batch_status"], "full_solver_status": solver["full_solver_status"],
               "paper_full_status": ref["full_status"], "lb1_seconds": solver["lb1_seconds"]}
        proposed = Decimal(raw["proposed_objective"])
        for measure, benchmark in (("gap_lb_pct", "lower_bound"), ("delta_full_pct", "full_objective"),
                                   ("delta_ds_pct", "ds_objective"), ("delta_pb_pct", "pb_objective")):
            row[measure] = ""
            if raw[benchmark]:
                baseline = Decimal(raw[benchmark])
                denominator = proposed if measure == "gap_lb_pct" else baseline
                percent = (proposed - baseline) * 100 / denominator
                row[measure] = str(percent.quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))
            row["match_" + measure] = same_number(row[measure], ref[measure])
        for field, value in raw.items():
            row["match_" + field] = same_number(value, ref[field], 2 if field.endswith("_seconds") else None)
        row["match_ds_best_to_paper"] = same_number(row["ds_best_objective"], ref["ds_objective"])
        row["match_full_batch_status"] = STATUS.get(row["full_batch_status"]) == ref["full_status"]
        row["match_full_solver_status"] = STATUS.get(row["full_solver_status"]) == ref["full_status"]
        outcomes.append(row)
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate results/verified_results.csv")
    args = parser.parse_args()
    rows = compare()
    path = RESULTS / "verified_results.csv"
    if args.write:
        with path.open("w", encoding="utf-8", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)
    else:
        existing = read_rows(path)
        expected = {row["instance_id"]: {k: str(v) for k, v in row.items()} for row in rows}
        if existing != expected:
            raise SystemExit("The saved comparison differs. Inspect the source rows before regenerating.")
    for field in rows[0]:
        if field.startswith("match_"):
            count = sum(row[field] for row in rows)
            print(f"{field[6:]:26} {count}/{len(rows)}")
    print("Sources and saved comparison agree.")


if __name__ == "__main__":
    main()
