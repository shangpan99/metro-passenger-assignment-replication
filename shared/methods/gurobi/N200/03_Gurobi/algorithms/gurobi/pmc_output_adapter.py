"""PMC-compatible files from actual selected MILP trajectories; no FIFO replay."""
from collections import defaultdict
import json
import math
import time
import pandas as pd
from output_schemas import SCHEMAS, ITERATION_COLUMNS


def save(output, name, rows, columns=None):
    pd.DataFrame(rows, columns=columns or SCHEMAS[name]).to_csv(
        output/name,index=False,encoding='utf-8-sig')


def event_audit(data, columns, agents, selected):
    ranks={a.agent_id:a.rank for a in agents}
    occupants=defaultdict(set)
    boarders=defaultdict(list)
    episodes=[]
    events=sorted(data.train_events,key=lambda e:(e.train_id,e.stop_index))
    trains={t.train_id:t for t in data.trains}
    for a in agents:
        c=columns[selected[a.agent_id]]
        if c.od_id!=a.od_id:
            raise ValueError('Selected column has wrong OD')
        for res in c.resources:
            occupants[res].add(a.agent_id)
        for ep in c.boarding_episodes:
            ev=data.event_by_resource[ep.first_resource]
            key=(ep.selected_departure_time,ep.selected_train_id,ev.stop_index)
            if ep.selected_departure_time<ep.ready_time-1e-9:
                raise ValueError('Boarding before ready time')
            record=(a.agent_id,ep,key)
            episodes.append(record)
            boarders[ep.first_resource].append(record)
    temporal,spatial,loads=[],[],[]
    for eid,ev in enumerate(events):
        tr=trains[ev.train_id];i=ev.stop_index;res=ev.outgoing_resource
        onboard=occupants[res]
        actual_records=sorted(boarders[res],key=lambda x:(x[1].ready_time,ranks[x[0]],x[0]))
        actual=tuple(x[0] for x in actual_records)
        through=onboard-set(actual)
        previous=occupants[(ev.train_id,tr.link_sequence[i-1])] if i else set()
        if not through<=previous:
            raise ValueError('Through passenger absent from previous segment')
        residual=ev.capacity-len(through)
        order=(ev.departure_time,ev.train_id,i);suffix=tuple(tr.link_sequence[i:])
        waiting=[x for x in episodes if x[1].platform_node==ev.platform_node
                 and x[1].ready_time<=ev.departure_time and x[2]>=order
                 and x[1].block_links==suffix[:len(x[1].block_links)]]
        waiting.sort(key=lambda x:(x[1].ready_time,ranks[x[0]],x[0]))
        prefix=tuple(x[0] for x in waiting[:max(0,int(residual))])
        common=dict(event_id=eid,train_id=ev.train_id,stop_index=i,
                    platform_node=ev.platform_node,departure_time=ev.departure_time)
        if actual!=prefix[:len(actual)]:
            temporal.append(dict(**common,expected_fifo_prefix=';'.join(map(str,prefix)),
                actual_boarded_agents=';'.join(map(str,actual)),violation_type='FIFO_PREFIX_OR_QUEUE_RECORD'))
        expected=min(max(0,int(residual)),len(waiting))
        if len(actual)!=expected:
            spatial.append(dict(**common,violation_type='WORK_CONSERVING_BOARDING',
                expected_value=str(expected),actual_value=str(len(actual))))
        loads.append(dict(**common,outgoing_link_id=ev.outgoing_link_id,
            alight=len(previous-through),occ_minus_after_alighting=len(through),
            residual_boarding_capacity=residual,new_board=len(actual),
            occ_plus_after_boarding=len(onboard),train_capacity=ev.capacity))
    return temporal,spatial,loads


def finalize_outputs(data, columns, agents, indexes, sol, temporal_required, spatial_required,
                     output, started, weights):
    """Write the same nine detailed CSV schemas and final-only PMC summaries."""
    summary=dict(model=sol.model_name,status=sol.status_name,is_optimal=int(sol.is_optimal),
        has_incumbent=int(sol.has_incumbent),incumbent_objective=sol.incumbent_objective,
        best_bound=sol.best_bound,mip_gap=sol.mip_gap,gurobi_runtime_seconds=sol.gurobi_runtime,
        variables=sol.variable_count,binary_variables=sol.binary_count,constraints=sol.constraint_count,
        temporal_fifo_required=int(temporal_required),spatial_fifo_required=int(spatial_required))
    totals={key:None for key in ['total_travel_time','total_waiting_time','total_nonwaiting_time',
                               'total_early_time','total_late_time','schedule_cost']}
    full_feasible=False
    if sol.has_incumbent:
        if set(sol.selected_column_by_agent)!={a.agent_id for a in agents}:
            raise ValueError('Selected solution does not contain every passenger exactly once')
        chosen=[columns[sol.selected_column_by_agent[a.agent_id]] for a in agents]
        temporal,spatial,loads=event_audit(data,columns,agents,sol.selected_column_by_agent)
        save(output,'temporal_fifo_violations.csv',temporal)
        save(output,'spatial_fifo_violations.csv',spatial)
        save(output,'train_occupancy_residual_capacity.csv',loads)
        cap=pd.read_csv(output/'capacity_audit.csv')
        cap_count=int((cap.violation>1e-6).sum())
        summary.update(capacity_violations=cap_count,temporal_fifo_violations=len(temporal),
                       spatial_fifo_violations=len(spatial),
                       objective_reconstructed=math.fsum(c.unit_cost for c in chosen))
        if cap_count or (temporal_required and temporal) or (spatial_required and spatial):
            raise ValueError('Required model constraints failed PMC-format event audit')
        full_feasible=not(cap_count or temporal or spatial)
        totals.update(total_travel_time=math.fsum(c.total_travel_time for c in chosen),
            total_waiting_time=math.fsum(ep.selected_departure_time-ep.ready_time
                                        for c in chosen for ep in c.boarding_episodes),
            total_early_time=math.fsum(c.early_time for c in chosen),
            total_late_time=math.fsum(c.late_time for c in chosen),
            schedule_cost=math.fsum(c.unit_schedule_cost for c in chosen))
        totals['total_nonwaiting_time']=totals['total_travel_time']-totals['total_waiting_time']
        if not math.isclose(summary['objective_reconstructed'],sol.incumbent_objective,abs_tol=1e-5,rel_tol=0):
            raise ValueError('Reconstructed objective mismatch')
        # Stable column names/order for all common exports, even when tables are empty.
        for name, fields in SCHEMAS.items():
            if name=='model_summary.csv':continue
            table=pd.read_csv(output/name)
            if set(table.columns)!=set(fields):
                raise ValueError('Unexpected output columns in '+name)
            table[fields].to_csv(output/name,index=False,encoding='utf-8-sig')
    else:
        for name in SCHEMAS:
            if name!='model_summary.csv':save(output,name,[])
    elapsed=time.perf_counter()-started
    sol.complete_wall_time=elapsed
    summary['complete_wall_time_seconds']=elapsed
    summary={k:(None if isinstance(v,float) and not math.isfinite(v) else v) for k,v in summary.items()}
    save(output,'model_summary.csv',[summary])
    history=[]
    if sol.has_incumbent:
        history=[dict(iteration=0,accepted_by='GUROBI_FINAL_INCUMBENT',state_version=0,
                      objective=sol.incumbent_objective,agents=len(agents),full_feasible=full_feasible,
                      elapsed_seconds=elapsed,**totals)]
    save(output,'iteration_summary.csv',history,ITERATION_COLUMNS)
    report=dict(algorithm_version='GUROBI_REALIZED_PATH_MILP_PMC_OUTPUT_V1',
        model=sol.model_name,status=sol.status_name,has_incumbent=sol.has_incumbent,
        is_optimal=sol.is_optimal,final_objective=sol.incumbent_objective,
        best_bound=summary.get('best_bound'),mip_gap=summary.get('mip_gap'),
        complete_wall_time_seconds=elapsed,gurobi_runtime_seconds=sol.gurobi_runtime,
        final_total_travel_time=totals['total_travel_time'],
        final_total_waiting_time=totals['total_waiting_time'],
        final_total_nonwaiting_time=totals['total_nonwaiting_time'],
        final_schedule_cost=totals['schedule_cost'],
        objective_weights=weights,objective_mode='pure_tt' if weights==dict(travel_time=1,early=0,late=0) else 'weighted',
        final_feasible=sol.has_incumbent,full_fifo_feasible=bool(full_feasible),
        temporal_fifo_required=temporal_required,spatial_fifo_required=spatial_required,
        capacity_violations=summary.get('capacity_violations'),
        temporal_fifo_violations=summary.get('temporal_fifo_violations'),
        spatial_fifo_violations=summary.get('spatial_fifo_violations'),
        iteration_summary_scope='FINAL_INCUMBENT_ONLY_NOT_SOLVER_ITERATION_HISTORY',
        timing_scope='INPUT_READ_PATH_ENUMERATION_INDEXING_MODEL_BUILD_OPTIMIZE_AUDIT_AND_DETAILED_EXPORT',
        output_directory=str(output))
    (output/'run_summary.json').write_text(json.dumps(report,ensure_ascii=False,indent=2,allow_nan=False),encoding='utf-8')
