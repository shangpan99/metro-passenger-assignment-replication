# Gurobi full model and FIFO-relaxed lower bound

The two mixed-integer programs use the same train-specific trajectories, agent IDs, fixed FIFO ranks, timetables, and capacity constraints. `FULL` enforces temporal and spatial FIFO. `LB1` relaxes both FIFO constraints while retaining binary trajectory choices. The archived settings use one solver thread, seed 1, zero target MIP gap, no user-supplied MIP start, and an 1,800-second computation budget per instance and method.

## Running the models

Run these commands from `methods/gurobi`. The Python dependencies are NumPy, pandas, psutil, and gurobipy. Solving requires a Gurobi license that covers the model size. The shared inputs are packaged under `../proposed_pb/data/N100_C10`, `N200_C20`, `N400_C40`, and `N800_C80`. Keep the relative package layout when extracting the archive.

```sh
python -m pip install -r requirements.txt
python run.py --scale 100 --dry-run
python run.py --instance N0100_R01_W1 --method LB1 --output runs/example_lb1
python run.py --instance N0100_R01_W1 --method FULL --output runs/example_full
python run.py --scale 200 --method both --output runs/n200_both
```

`--scale` selects all ten demand realizations and four departure windows at that scale. `--dry-run` checks stored hashes of the selected inputs without invoking the solver. `--time-limit` changes the per-task time budget in seconds. Choose a new or empty output directory. Each task produces a console log, a batch-level `run_summary.json`, and the worker's own `run/run_summary.json`. The archived R01–W1 lower bound at N100 is 3,106 passenger-minutes. Wall-clock times depend on the machine and solver environment.

## Archived benchmark results

`historical_results.csv` has one row for each of the 160 paper instances. `full_status` and `full_batch_status` record the original batch status. `full_solver_status` records the worker's status. `full_objective` is the incumbent in passenger-minutes and is blank when unavailable. `full_seconds` is the original complete wall-clock time, including the detailed result export. `lower_bound` is the best bound recorded by the FIFO-relaxed solver. For these 160 instances the LB1 worker recorded optimality and identical objective and bound. The file also records the LB1 batch and worker statuses, elapsed time, and source result-directory identifiers.

The four `evidence/N*_source_rows.csv` files preserve the FULL and LB1 rows for W1–W4 from their respective original batch summaries. `evidence/source_index.csv` identifies each original package and summary and records the SHA-256 of that complete original summary. N100 and N200 came from the `B11_ShortestMiddle_TwoStep_NoWarmStart` batches. N400 and N800 came from their dedicated `W1-W4_Gurobi_FULL_LB1_40Cases` batches.

Three FULL tasks have batch status `PROCESS_ERROR` and solver status `INTERRUPTED`. They are N0400_R05_W2, N0400_R07_W2, and N0800_R01_W4. Each solver finished without an incumbent before the worker encountered a file-permission error when saving its lifecycle marker. The manuscript lists these tasks as ERR, INT, and INT, respectively. N0400_R01_W3 is recorded as `TIME_LIMIT` by both the batch and worker, while the manuscript reports INT.

The LB1 task N0800_R04_W3 also encountered a file-permission error when saving its lifecycle marker. Its batch status is `PROCESS_ERROR`. Its completed worker summary records `OPTIMAL`, objective 24,328, best bound 24,328, and zero gap. The solver log independently confirms the objective and bound. The published lower bound uses this solver record.

The five console logs in this distribution are excerpts. They retain the original solver version, termination messages, solution counts and objective/bound lines. Computer-specific paths and license messages are omitted. The packaged model reads hash-verified data from the shared input directory.
