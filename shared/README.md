# System-optimal passenger assignment on a stylized metro network

This replication package covers the four demand scales in *System-Optimal Dynamic Passenger Assignment in Schedule-Based Metro Networks with Hard Capacity and Exact FIFO Constraints*. Each scale has ten demand realizations (R01–R10) and four departure windows (W1–W4). The 160 instances have 100, 200, 400 or 800 passengers and corresponding train capacities of 10, 20, 40 or 80. The objective is total passenger travel time in passenger-minutes. A smaller value is better.

## Read the results

| File | Contents |
| --- | --- |
| [Paper tables](results/paper_tables.csv) | The 160 rows as printed in the supplied manuscript. An empty full-MILP objective represents the printed dash. Percentage fields are in percentage points. |
| [Verified results](results/verified_results.csv) | One row per instance with archived objectives, original runtimes in seconds, solver statuses and column-by-column comparisons to the paper. |
| [Proposed and PB-PMC](methods/proposed_pb/README.md) | C++ program, inputs, build instructions and historical results. PB-PMC is called `Approximate_PMC` or SAR-PMC in the code. |
| [DS-PMC](methods/ds_pmc/README.md) | Historical stopping states, best observed values, archived complete runtimes and instructions for the `Constant_PMC_DiminishingStep` implementation. |
| [Full MILP and FIFO-relaxed MILP](methods/gurobi/README.md) | Gurobi model sources, runner, historical solver bounds and exceptional termination records. `FULL` enforces FIFO. `LB1` supplies the FIFO-relaxed bound. |

## Verify without solving

From the extracted repository root, run the following command with Python 3.8 or later. It uses only the Python standard library and finishes without a solver license. It checks the saved comparison against the method summaries. To rebuild the comparison after inspecting a source change, append `--write`.

```sh
python verify_results.py
```

For every method, the archived objective or bound agrees with the paper on all 160 rows when the DS-PMC stopping value is used. For the full MILP, agreement includes instances where no incumbent was reported. For each of the four methods with published times, the complete runtime in the original output rounds to the printed time on all 160 rows. The paper does not print a separate runtime column for LB1. Reruns produce new timings.

## Interpret the archived values

- **Proposed relocation depth.** The manuscript defines B=10 as the limit on relocation depth. The archived implementation also visits depth 11. Its single-move loop admits improving candidates at that depth: N0800_R03_W3 records an accepted SINGLE update at order 11. Seven other order-11 records are TWO_STEP updates whose first depth field stores the search limit. The archived run therefore cannot certify a strict relocation limit of 10.
- **DS-PMC objective.** All paper comparisons use the assignment at the final DS-PMC iteration (final_objective). The best objective visited is retained as a diagnostic; it differs from the final objective on 21 of 160 instances.
- **Gurobi statuses.** The batch and solver can finish at different stages. Both status fields are preserved. For N0400_R01_W3 the batch and solver report `TIME_LIMIT`, while the paper shows `INT`. N0400_R05_W2, N0400_R07_W2 and N0800_R01_W4 have batch status `PROCESS_ERROR` after solver status `INTERRUPTED`. The paper reports ERR, INT and INT respectively. The numerical comparisons use the archived incumbent, bound and time irrespective of these labels.
- **Recovered lower bound.** For N0800_R04_W3, the batch summary lacks the LB1 result after a finalization error. The solver output records an optimal bound of 24328. The relevant evidence is included with the Gurobi results.

## Run the experiments

These commands run all five methods on one example from the repository root on Windows. Install the Gurobi dependencies and activate your own license before the second command.

```powershell
python methods/proposed_pb/run_paper.py --method all --case N0100_R01_W2 --output runs/heuristics_example
python methods/gurobi/run.py --instance N0100_R01_W2 --method both --output runs/milp_example
```

For all 160 instances, use `--case ALL` for the three heuristics. For the two MILPs, run the following four batches, each in a new directory.

```powershell
foreach ($n in 100,200,400,800) { python methods/gurobi/run.py --scale $n --method both --output "runs/milp_$n" }
```

Use the linked method instructions for exact build commands, dependencies and one-instance examples. The heuristics use C++17. The two mathematical programs require Python, Gurobi and a license that supports the model size. The historical solver logs identify Gurobi 11.0.2 on Windows. The paper sets a 1,800-second budget per method and instance. The original batch summaries may also list W5. The paper comparison selects only W1–W4. Fresh solver outputs belong in a new output folder, so the historical records remain available for comparison.

## 中文使用说明

查看论文数字请打开 [逐例核对表](results/verified_results.csv)。每行对应一个规模、一个需求样本和一个出发时间窗。`match_*` 列表示归档结果是否与[论文表格](results/paper_tables.csv)一致。运行 `python verify_results.py` 可以重新核对，查看历史结果无需安装 Gurobi。

复跑时按上面三份方法说明操作。在 Windows 上可解压到较短的目录，如 `D:\metro`，并给新结果指定较短的输出路径。C++ 程序会为每个实例生成多层子目录，过长的路径可能导致导出失败。解释结果时尤其要区分所提算法的深度设置、DS-PMC 的终止值与历史最好值，以及 Gurobi 的批次状态与求解器状态。已逐例保留这些差异。
