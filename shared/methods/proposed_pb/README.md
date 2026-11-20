# 论文小算例的 C++ 程序与结果

`cpp_results.csv` 可用于逐实例查阅 Proposed 与 PB-PMC 的目标值和耗时。本文的 4 个规模分别为 100、200、400、800 人，每个规模包含 R01–R10、W1–W4，共 160 个实例。与论文逐行核对后，两种方法的目标值和保留两位小数的完整耗时均为 160/160 行吻合。

## 程序与原始结果

| 论文方法 | 归档程序 | 文件里的方法名 | 结果口径 |
| --- | --- | --- | --- |
| Proposed | `two_methods` | `Proposed` | `final_objective` |
| PB-PMC | `two_methods` | `Approximate_PMC`，命令行用 `sar` | `final_objective` |
| DS-PMC | `three_methods` | `Constant_PMC_DiminishingStep`，命令行用 `constant` | 原始表含 `final_objective` 与 `best_objective`，两列都应保留 |

`two_methods` 来自原目录 `IJOC_Multiscale_CPP`，`three_methods` 来自 `IJOC_Multiscale_CPP_ThreeMethods`。前者生成归档的 Proposed 和 PB-PMC 结果，后者生成 DS-PMC 结果。原始批次为串行，每实例每方法的计算预算为 1,800 秒。两套程序共用 `data` 下的实例、时刻表和初始分配。

`archived_results/experiment_summary.csv` 保留双方法原批次的 200 个实例，含 W5。论文只使用 W1–W4。每个保留的实例可以在 `archived_results/<实例编号>/<方法名>/` 查找 `run_summary.json`。其 `raw` 子目录含逐轮记录 `iteration_summary.csv`、优化后的路径和出发时间流量 `optimized_path_departure_flow.csv`，以及逐乘客时刻表 `output_agent_reservation_timetable_optimized.csv`。比较耗时采用 `complete_wall_time_seconds`，单位为秒，包含算法收尾和结果导出。新运行的耗时会随机器和系统负载变化。

The manuscript defines B=10 as the limit on relocation depth. The archived implementation also visits depth 11. Its single-move loop admits improving candidates at that depth: N0800_R03_W3 records an accepted SINGLE update at order 11. Seven other order-11 records are TWO_STEP updates whose first depth field stores the search limit. The archived run therefore cannot certify a strict relocation limit of 10. All paper comparisons use the assignment at the final DS-PMC iteration (final_objective). The best objective visited is retained as a diagnostic; it differs from the final objective on 21 of 160 instances.

## 复现

在此目录打开终端。下列命令使用 Python 3 调用随附的 Windows 程序。Python 入口只用标准库；C++ 小算例的这三种方法均可独立运行。也可从其他位置以完整路径调用 `run_paper.py`。相对输出路径以调用时的当前目录为准，请为每次求解指定新的目录。

Windows 上宜选较短的输出路径，例如 `D:/metro_runs/run01`。DS-PMC 会生成多层明细目录，在很深的输出位置可能触及原版程序的路径长度限制并报 `iostream stream error`。

```powershell
python run_paper.py --method both --case N0100_R01_W2 --dry-run
python run_paper.py --method both --case N0100_R01_W2 --output runs/check_R01_W2
python run_paper.py --method ds --case N0100_R01_W2 --output runs/ds_R01_W2
```

该实例的归档目标值为 Proposed **3,167**、PB-PMC **3,238**、DS-PMC **3,230**。本次运行的结果写入所选目录下的 `comparison_instances.csv`、`experiment_summary.csv` 和逐实例子目录。

运行论文全部 160 个实例、全部三种方法：

```powershell
python run_paper.py --method all --case ALL --output runs/paper_160
```

`--method` 也接受 `proposed`、`pb`、`ds` 或 `both`。其中 `both` 运行 Proposed 和 PB-PMC，`all` 会在输出下分别建立 `two_methods`、`three_methods` 子目录。每个实例使用独立保存的输入和初始状态。`--dry-run` 逐实例校验输入、完整重放初始 FIFO 分配。

Windows 可在 Visual Studio x64 开发者命令提示符下，进入对应版本目录运行 `build_windows.bat`。Linux 可在对应版本目录运行下面的命令。入口优先使用该版本 `build/Release` 或 `build` 中的新编译文件，其次使用随包程序。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```
