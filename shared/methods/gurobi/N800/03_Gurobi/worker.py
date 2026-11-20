"""Run the original FULL/LB1 MILP with bundled inputs and NO MIP start."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import traceback
import bench_runtime as bench

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--method', choices=['FULL', 'LB1'], required=True)
    parser.add_argument('--instance', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--time-limit', type=float, default=1800)
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT))
    from common import DATA
    inputs = DATA / 'instances' / args.instance
    if not inputs.is_dir() or inputs.parent.resolve() != (DATA / 'instances').resolve():
        raise ValueError('Unknown instance')
    if not math.isfinite(args.time_limit) or args.time_limit <= 0:
        raise ValueError('Time limit must be finite and positive')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise FileExistsError('Worker output must be empty')
    from common import verify_case, read_csv
    bench.configure(output, args.time_limit)
    initial_hash = None
    result = {}
    code = 0
    try:
        metadata = verify_case(args.instance)
        initial = DATA / 'initializations' / args.instance
        initial_hash = metadata['initial_agents_sha256']
        config = json.loads((ROOT / 'experiment_settings.json').read_text(encoding='utf-8-sig'))
        config['computation_time_limit_seconds'] = args.time_limit
        os.environ['FLOW_PMC_ROOT'] = str(inputs)
        os.environ.pop('BENCH_MIP_START', None)
        sys.path.insert(0, str(HERE / 'algorithms/gurobi'))
        import solve_two_agent_models as model
        model.BASE_DIR = inputs
        for key, name in [('INPUT_LINK', 'input_link.csv'), ('INPUT_NODE', 'input_node.csv'),
                          ('INPUT_PATH', 'input_path.csv'), ('INPUT_DEMAND', 'input_demand.csv'),
                          ('INPUT_TRAIN_TIMETABLE', 'input_train_timetable.csv')]:
            setattr(model, key, inputs / name)
        model.GUROBI_THREADS = config['gurobi_threads']
        model.GUROBI_TIME_LIMIT = args.time_limit
        model.GUROBI_SEED = config['gurobi_seed']
        model.GUROBI_MIP_GAP = config['gurobi_mip_gap']
        model.WRITE_GUROBI_LP_FILES = False
        raw = output / 'raw'
        bench.save_json(output / 'config_used.json', config)
        bench.save_json(output / 'lifecycle.json', dict(stage='computation', started_unix=time.time()))
        bench.check()
        bench.set_phase('input_loading')
        data = model.read_input_data()
        columns, cache = model.generate_realized_paths(data)
        records = read_csv(initial / 'initial_agents.csv')
        agents = [model.AgentData(agent_id=int(r['agent_id']), od_id=int(r['od_id']),
                                  rank=int(r['fixed_rank'])) for r in records]
        from collections import Counter
        observed = Counter(a.od_id for a in agents)
        if any(observed[od] != d.demand for od, d in data.demands.items()):
            raise ValueError('Saved agents and aggregate OD demand disagree')
        bench.save_json(output / 'agent_identity_check.json', dict(matched=True,
            passengers=len(agents), preserved_ids_and_fifo_ranks=True))
        indexes = model.build_model_indexes(data, columns, cache)
        bench.save_json(output / 'model_preprocessing_summary.json', dict(
            realized_path_columns=len(columns), passengers=len(agents),
            preprocessing_seconds=time.perf_counter() - bench.started))
        name, temporal, spatial, _ = model.MODEL_SPECS[args.method]
        solution = model.solve_model(data, columns, agents, indexes, name, temporal, spatial,
                                     raw, complete_started=bench.started)
        summary = raw / 'run_summary.json'
        if summary.exists():
            result = json.loads(summary.read_text(encoding='utf-8-sig'))
        result.update(status=solution.status_name, final_objective=solution.incumbent_objective,
                      is_optimal=solution.is_optimal, has_incumbent=solution.has_incumbent,
                      best_bound=solution.best_bound, mip_gap=solution.mip_gap,
                      variables=solution.variable_count, binary_variables=solution.binary_count,
                      constraints=solution.constraint_count, gurobi_runtime_seconds=solution.gurobi_runtime)
    except bench.ComputationTimeout as exc:
        result = dict(status='TIME_LIMIT_BEFORE_SOLVE', has_incumbent=False, detail=str(exc))
    except Exception as exc:
        traceback.print_exc()
        result = dict(status='ERROR', has_incumbent=False, error=type(exc).__name__ + ': ' + str(exc))
        code = 1
    bench.finish_computation()
    result.update(bench.finalize_metrics())
    result.update(method=args.method, instance_id=args.instance,
                  initial_objective=None, initialization_rule='NO_MIP_START',
                  initial_assignment_sha256=initial_hash, mip_start_supplied=False, mip_start_used=False,
                  input_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in sorted(inputs.glob('input_*.csv'))},
                  computation_time_limit_seconds=args.time_limit, local_optimum_certified=False,
                  global_optimum_proven=bool(result.get('is_optimal') and args.method == 'FULL'))
    bench.save_json(output / 'run_summary.json', result)
    bench.save_json(output / 'termination_summary.json', dict(status=result['status'],
                    global_optimum_proven=result['global_optimum_proven'], error=result.get('error')))
    bench.save_json(output / 'lifecycle.json', dict(stage='done'))
    print('FINISHED', args.instance, args.method, result['status'], result.get('final_objective'), flush=True)
    return code


if __name__ == '__main__':
    raise SystemExit(main())
