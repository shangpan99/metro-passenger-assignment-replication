# Stylized metro network replication
## Compare the paper results
| Passengers | Train capacity | Comparison across five methods |
|---:|---:|---|
| 100 | 10 | [40 instances](100/comparison.csv) |
| 200 | 20 | [40 instances](200/comparison.csv) |
| 400 | 40 | [40 instances](400/comparison.csv) |
| 800 | 80 | [40 instances](800/comparison.csv) |

Each scale contains five peer directories: LB, Gurobi (full model), DS-PMC, PB-PMC and Proposed method. Each contains results.csv and a run.py entry point. Join rows by instance_id (R01-R10, W1-W4). Objectives and bounds are passenger-minutes; runtimes are seconds. A blank full-model objective indicates no archived incumbent.

## Reproduce an instance
From the repository root:
```sh
python "100/Proposed method/run.py" N0100_R01_W2 --output runs/example
python "200/LB/run.py" N0200_R01_W1 --output runs/lb_example
python shared/verify_results.py
```
Use a fresh output path. For build instructions, platform requirements, Gurobi licensing, historical output and full batches see the [original replication guide](shared/README.md). The shared folder preserves the original sources, inputs, result evidence and verifier with their relative paths intact.
