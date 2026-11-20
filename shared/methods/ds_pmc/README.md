# Diminishing-step PMC (DS-PMC)

The C++ method identifier is `Constant_PMC_DiminishingStep`. The [three-method C++17 source and executable](../proposed_pb/three_methods/) use the [shared input data](../proposed_pb/data/). Archived results in this folder come from the serial run with a 1,800-second time limit and a 1,000-attempt cap.

## Recover the paper's DS-PMC columns

[paper_comparison.csv](paper_comparison.csv) matches the 160 rows in the four stylized-network tables by passenger count, repetition and departure window. All 160 printed objectives match `final_objective`. All 160 printed times match `complete_wall_time_seconds` rounded to two decimal places.

All paper comparisons use the assignment at the final DS-PMC iteration (final_objective). The best objective visited is retained as a diagnostic; it differs from the final objective on 21 of 160 instances.

| Passengers | Paper cases | Final objective matches | Best observed matches | Rounded time matches |
|---:|---:|---:|---:|---:|
| 100 | 40 | 40 | 39 | 40 |
| 200 | 40 | 40 | 34 | 40 |
| 400 | 40 | 40 | 30 | 40 |
| 800 | 40 | 40 | 36 | 40 |

## Run one case

From the shared `methods/proposed_pb` folder, run the following in Windows PowerShell. The output directory is relative to that folder. Use a different output directory for a different selection. For Windows installations with path-length limits, extracting the package near the drive root keeps the nested filenames short.

```powershell
.\three_methods\run_all_scales.exe --data .\data --method constant --serial --time-limit 1800 --constant-max-iterations 1000 --cases N0100_R01_W3 --output outputs/ds_single
```

For all 160 paper cases, use the shared Python launcher from the same folder. It selects W1–W4 across the four scales and ten repetitions, and writes to a fresh output directory.

```powershell
python run_paper.py --method ds --output outputs/ds_paper160
```

## Original records

- [comparison_instances.csv](archived_results/comparison_instances.csv) contains the original batch summary, with both objectives, the iteration of the best objective, the complete runtime, the termination status and cycle flag. The batch also includes W5. The paper uses W1–W4.
- [experiment_summary.csv](archived_results/experiment_summary.csv), [all_run_summaries.json](archived_results/all_run_summaries.json) and [batch_config.json](archived_results/batch_config.json) preserve the source's detailed summary and settings.
- For each of the 160 paper cases, `archived_results/<instance>/Constant_PMC_DiminishingStep/` contains the original `run_summary.json`, input fingerprint and initialization metadata. Its `raw` directory contains the attempt history, cycle report, iteration summary, final passenger assignments and best-observed passenger assignments. Each row of the comparison file names its supporting record.

The source project on the original machine is `D:\IJOC_TEST\IJOC_Multiscale_CPP_ThreeMethods`. The source batch is `outputs/multiscale_cpp/200cases_ebb1c17087bf809f_serial_Constant_PMC_DiminishingStep`. The SHA-256 digest of its unchanged `comparison_instances.csv` is `a0f01ceb4668d34ee99ed0bc03c22147b3e1651969a20da1618747ae1d378750`.
