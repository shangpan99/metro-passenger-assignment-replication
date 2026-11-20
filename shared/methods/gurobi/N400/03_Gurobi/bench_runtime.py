"""Experiment instrumentation only; no move selection or objective modification."""
import csv
import functools
import json
import math
import os
from pathlib import Path
import time
from contextlib import contextmanager

class ComputationTimeout(BaseException):
    pass

started = time.perf_counter()
deadline = math.inf
output = None
active = False
phase = 'setup'
phase_started = started
phases = {}
counts = {}
last_state = None
initial_objective = None
progress = []
computation_seconds = None


def configure(directory, limit):
    global started, deadline, output, active, phase_started
    started = time.perf_counter()
    deadline = started + float(limit)
    output = Path(directory)
    output.mkdir(parents=True, exist_ok=True)
    phase_started = started
    active = True


def clean(value):
    if isinstance(value, dict): return {str(k): clean(v) for k,v in value.items()}
    if isinstance(value, (list,tuple)): return [clean(v) for v in value]
    if hasattr(value, 'item'): return clean(value.item())
    if isinstance(value,float) and not math.isfinite(value): return None
    if isinstance(value,Path): return str(value)
    return value


def save_json(path, value):
    path=Path(path); path.parent.mkdir(parents=True,exist_ok=True)
    temp=path.with_suffix(path.suffix+'.tmp')
    temp.write_text(json.dumps(clean(value),ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
    temp.replace(path)


def write_csv(path, rows, fields=None):
    path=Path(path);path.parent.mkdir(parents=True,exist_ok=True)
    fields=fields or list(dict.fromkeys(k for r in rows for k in r))
    with path.open('w',newline='',encoding='utf-8-sig') as f:
        w=csv.DictWriter(f,fieldnames=fields,extrasaction='ignore');w.writeheader()
        w.writerows(clean(rows))


def check():
    if active and time.perf_counter() >= deadline:
        raise ComputationTimeout('Computation time budget exhausted during '+phase)


def remaining():
    check()
    return max(0.001,deadline-time.perf_counter()) if active else math.inf


def set_phase(name):
    global phase,phase_started
    now=time.perf_counter()
    phases[phase]=phases.get(phase,0.0)+now-phase_started
    phase=name;phase_started=now


@contextmanager
def scope(name):
    old=phase;set_phase(name)
    try: yield
    finally: set_phase(old)


def timed(name):
    def decorate(fn):
        @functools.wraps(fn)
        def call(*args,**kwargs):
            check();counts[name]=counts.get(name,0)+1
            with scope(name): return fn(*args,**kwargs)
        return call
    return decorate


def finish_computation():
    global active,computation_seconds
    if computation_seconds is None:
        computation_seconds=time.perf_counter()-started
    active=False;set_phase('final_export')
    if output:
        save_json(output/'lifecycle.json',dict(stage='final_export',computation_seconds=computation_seconds,
                  export_started_unix=time.time()))


def observe(frame,snapshot):
    """Retain the latest fully accepted immutable snapshot, never a trial."""
    global last_state,initial_objective
    if not getattr(snapshot,'feasible',False): return
    value=float(snapshot.objective)
    if initial_objective is None: initial_objective=value
    if last_state is None or snapshot is not last_state[1]:
        last_state=(frame.copy(deep=True),snapshot)
        if not progress or abs(value-progress[-1]['objective'])>1e-9:
            record_progress(value,record_type='ACCEPTED_STATE')


def record_progress(value=None,bound=None,record_type='SOLVER',force=False):
    now=time.perf_counter()-started
    row=dict(elapsed_seconds=now,objective=value,best_bound=bound,record_type=record_type)
    if not force and record_type=='SOLVER' and progress:
        prev=progress[-1]
        if now-prev['elapsed_seconds']<1 and value==prev['objective']: return
    progress.append(row)
    if output:
        f=output/'progress_trace.csv'; exists=f.exists()
        with f.open('a',newline='',encoding='utf-8') as stream:
            w=csv.DictWriter(stream,fieldnames=list(row))
            if not exists:w.writeheader()
            w.writerow(clean(row))


def optimize(model):
    """Global-model callback records actual incumbent/bound history."""
    import gurobipy as gp
    model.Params.TimeLimit=min(float(model.Params.TimeLimit),remaining())
    def callback(m,where):
        if where==gp.GRB.Callback.MIP:
            v=m.cbGet(gp.GRB.Callback.MIP_OBJBST);b=m.cbGet(gp.GRB.Callback.MIP_OBJBND)
            record_progress(None if abs(v)>=gp.GRB.INFINITY else v,
                            None if abs(b)>=gp.GRB.INFINITY else b)
        if active and time.perf_counter()>=deadline:m.terminate()
    with scope('solver_optimization'):model.optimize(callback)
    record_progress(float(model.ObjVal) if model.SolCount else None,
                    float(model.ObjBound) if model.IsMIP else None,'SOLVER_FINAL',True)


def apply_mip_start(model,z,columns,agents):
    """No user-supplied MIP start is applied in this experiment package."""
    return


def finalize_metrics():
    set_phase('done')
    if output:
        write_csv(output/'phase_timing.csv',[dict(phase=k,seconds=v,calls=counts.get(k)) for k,v in phases.items()])
    return dict(computation_seconds=computation_seconds,
                complete_wall_time_seconds=time.perf_counter()-started,
                phase_seconds=dict(phases),instrumentation_counts=dict(counts))
