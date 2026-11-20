# -*- coding: utf-8 -*-
"""
Two agent-based passenger-assignment models under two FIFO dimensions
======================================================================

This standalone script solves two models from the SAME five input files and
SAME train-specific realized-path pool:

    Model FULL
        Temporal FIFO  : retained
        Spatial FIFO   : retained

    Model LB1
        Temporal FIFO  : relaxed
        Spatial FIFO   : relaxed

FIFO terminology used here
--------------------------
Temporal FIFO (queue-order FIFO)
    At the same line-direction platform, entry and transfer passengers join ONE
    common queue. Passengers earlier in the queue cannot board later than
    passengers behind them. A deterministic agent rank breaks same-time ties.

Spatial FIFO (work-conserving / mandatory boarding)
    If a passenger is already ready at a platform and an earlier eligible train
    is skipped, then that skipped train must leave full. Equivalently, a
    passenger may remain for a later train only because the current eligible
    train has exhausted its capacity.

Modeling strategy
-----------------
1) Enumerate all timetable-feasible train-specific realized paths consistent
   with each OD, physical path, and admissible departure time.
2) Create unit passenger agents. Each agent chooses exactly one realized path.
3) Impose train-segment capacity in all two models.
4) Spatial FIFO, when active, is enforced by event-level work-conserving
   constraints: if any selected trajectory skips an earlier eligible train,
   that train's post-boarding occupancy must equal capacity.
5) Temporal FIFO, only in FULL, is enforced WITHOUT passenger-pair precedence
   binaries. A compact platform-event FIFO frontier ensures that after every
   relevant departure event, boarded passengers form a prefix of the unified
   platform queue ordered by (ready_time, deterministic_agent_rank).

Outputs
-------
Each model writes, among other files:
    output_agent_reservation_timetable_optimized.csv
with the same main trajectory fields used by the Flow-Agent implementation:
    agent_id,node_sequence,link_sequence,time_sequence,
    early_time,delay_time,time_cost,volume,total_travel_time

The script is designed for direct IDE/double-click execution. No command-line
arguments are required.
"""

from __future__ import annotations
import bench_runtime as bench

from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple, Optional, Set
import csv
import math
import os
import time

import pandas as pd


# =============================================================================
# 0. USER SETTINGS
# =============================================================================

BASE_DIR = Path(__file__).resolve().parent

INPUT_LINK = BASE_DIR / "input_link.csv"
INPUT_NODE = BASE_DIR / "input_node.csv"
INPUT_PATH = BASE_DIR / "input_path.csv"
INPUT_DEMAND = BASE_DIR / "input_demand.csv"
INPUT_TRAIN_TIMETABLE = BASE_DIR / "input_train_timetable.csv"

OUTPUT_ROOT = BASE_DIR / "two_agent_model_results"

# Objective parameters, kept consistent with the current stylized Flow-Agent.
THETA = 1
BETA = 0
GAMMA = 0

# Gurobi settings.
GUROBI_OUTPUT_FLAG = 1
GUROBI_THREADS = 0          # 0 = automatic
GUROBI_TIME_LIMIT = 1800.0  # worker also deducts model preprocessing from this budget
GUROBI_MIP_GAP = 0.0
GUROBI_MIP_FOCUS = 1
GUROBI_HEURISTICS = 0.30
GUROBI_SYMMETRY = 2
GUROBI_PRESOLVE = 2
GUROBI_SEED = 1

# LP/MPS export is diagnostic only. On Windows, older Gurobi versions may
# fail when Model.write() receives a full path containing non-ASCII characters.
# Keep this optional and never let a diagnostic export abort optimization.
WRITE_GUROBI_LP_FILES = True

# Numerical tolerances.
TIME_TOL = 1e-9
FLOW_TOL = 1e-7
CAPACITY_TOL = 1e-6
OBJECTIVE_TOL = 1e-5
FIFO_TOL = 1e-6

# Current timetable is minute-discretized. Temporal FIFO uses an integer key
# (ready_time, agent_rank), so all queue-ready times must lie on this grid.
TIME_GRID = 1.0

# Link-type convention in the supplied data.
LINK_TYPE_ACCESS = 1
LINK_TYPE_EGRESS = 2
LINK_TYPE_RUN = 3
LINK_TYPE_TRANSFER = 5


# =============================================================================
# 1. DATA STRUCTURES
# =============================================================================

Resource = Tuple[int, int]  # (train_id, physical run link_id)


@dataclass(frozen=True)
class NodeData:
    node_id: int
    node_type: int
    line_id: Optional[int]


@dataclass(frozen=True)
class LinkData:
    link_id: int
    from_node_id: int
    to_node_id: int
    travel_time: float
    capacity: float
    link_type: int
    description: str


@dataclass(frozen=True)
class PhysicalPath:
    path_id: int
    from_node_id: int
    to_node_id: int
    node_list: Tuple[int, ...]
    link_list: Tuple[int, ...]
    free_travel_time: float


@dataclass(frozen=True)
class DemandData:
    od_id: int
    origin: int
    destination: int
    demand: int
    departure_time_earliest: int
    departure_time_latest: int
    path_id_list: Tuple[int, ...]
    arrival_time_earliest: float
    arrival_time_latest: float


@dataclass(frozen=True)
class TrainService:
    train_id: int
    line_id: int
    node_sequence: Tuple[int, ...]
    link_sequence: Tuple[int, ...]
    time_sequence: Tuple[float, ...]
    capacity: float


@dataclass(frozen=True)
class TrainEvent:
    event_id: int
    train_id: int
    stop_index: int
    platform_node: int
    departure_time: float
    outgoing_resource: Resource
    outgoing_link_id: int
    capacity: float
    line_id: int


@dataclass(frozen=True)
class RideOption:
    train_id: int
    block_links: Tuple[int, ...]
    departure_time: float
    arrival_time: float
    node_times: Tuple[float, ...]
    resources: Tuple[Resource, ...]
    first_resource: Resource


@dataclass(frozen=True)
class BoardingEpisode:
    platform_node: int
    queue_entry_link_id: int
    block_links: Tuple[int, ...]
    ready_time: float
    selected_train_id: int
    selected_departure_time: float
    selected_arrival_time: float
    first_resource: Resource


@dataclass(frozen=True)
class RealizedPathColumn:
    column_id: int
    od_id: int
    path_id: int
    passenger_departure_time: float
    final_arrival_time: float
    total_travel_time: float
    early_time: float
    late_time: float
    unit_schedule_cost: float
    unit_cost: float
    train_sequence: Tuple[int, ...]
    resources: Tuple[Resource, ...]
    boarding_episodes: Tuple[BoardingEpisode, ...]
    node_sequence: Tuple[int, ...]
    link_sequence: Tuple[int, ...]
    time_sequence: Tuple[float, ...]


@dataclass(frozen=True)
class AgentData:
    agent_id: int
    od_id: int
    rank: int


@dataclass
class InputData:
    nodes: Dict[int, NodeData]
    links: Dict[int, LinkData]
    paths: Dict[int, PhysicalPath]
    demands: Dict[int, DemandData]
    trains: List[TrainService]
    resource_capacity: Dict[Resource, float]
    train_events: List[TrainEvent]
    event_by_resource: Dict[Resource, TrainEvent]
    events_by_platform: Dict[int, List[TrainEvent]]


@dataclass
class ModelSolution:
    model_name: str
    status_name: str
    is_optimal: bool
    has_incumbent: bool
    incumbent_objective: Optional[float]
    best_bound: Optional[float]
    mip_gap: Optional[float]
    gurobi_runtime: float
    complete_wall_time: float
    selected_column_by_agent: Dict[int, int]
    variable_count: int
    binary_count: int
    constraint_count: int


# =============================================================================
# 2. BASIC PARSING / VALIDATION
# =============================================================================


def _read_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"Input file not found: {path}")
    last_error = None
    for enc in ("utf-8-sig", "utf-8", "gb18030", "gbk"):
        bench.check()
        try:
            return pd.read_csv(path, encoding=enc)
        except Exception as exc:
            last_error = exc
    raise RuntimeError(f"Cannot read {path}: {last_error}")


_MISSING_TEXT_TOKENS = {"", "none", "nan", "null", "na", "n/a"}


def _is_missing_value(value) -> bool:
    """Return True for numeric NaN and common textual missing-value markers."""
    if pd.isna(value):
        return True
    return str(value).strip().lower() in _MISSING_TEXT_TOKENS


def _parse_optional_int(value, field_name: str = "value") -> int | None:
    """Parse an optional integer field robustly, including literal 'None'."""
    if _is_missing_value(value):
        return None
    text = str(value).strip()
    try:
        # Accept both "3" and spreadsheet-style "3.0".
        numeric = float(text)
        if not math.isfinite(numeric) or abs(numeric - round(numeric)) > 1e-9:
            raise ValueError
        return int(round(numeric))
    except Exception as exc:
        raise ValueError(
            f"Cannot parse optional integer field {field_name!r}: {value!r}"
        ) from exc


def _parse_int_list(value) -> Tuple[int, ...]:
    if _is_missing_value(value):
        return tuple()
    return tuple(
        int(float(tok.strip()))
        for tok in str(value).split(";")
        if tok.strip() != "" and tok.strip().lower() not in _MISSING_TEXT_TOKENS
    )


def _parse_float_list(value) -> Tuple[float, ...]:
    if _is_missing_value(value):
        return tuple()
    return tuple(
        float(tok.strip())
        for tok in str(value).split(";")
        if tok.strip() != "" and tok.strip().lower() not in _MISSING_TEXT_TOKENS
    )


def _require_columns(df: pd.DataFrame, required: Iterable[str], filename: str) -> None:
    missing = [c for c in required if c not in df.columns]
    if missing:
        raise ValueError(f"{filename} is missing required columns: {missing}")


def _is_grid_time(v: float) -> bool:
    return abs(v / TIME_GRID - round(v / TIME_GRID)) <= 1e-8


def _time_to_grid(v: float) -> int:
    if not _is_grid_time(v):
        raise ValueError(
            f"Temporal FIFO requires grid-aligned ready times; got {v}. "
            f"Current TIME_GRID={TIME_GRID}."
        )
    return int(round(v / TIME_GRID))


@bench.timed('input_loading')
def read_input_data() -> InputData:
    df_link = _read_csv(INPUT_LINK)
    df_node = _read_csv(INPUT_NODE)
    df_path = _read_csv(INPUT_PATH)
    df_demand = _read_csv(INPUT_DEMAND)
    df_train = _read_csv(INPUT_TRAIN_TIMETABLE)

    _require_columns(
        df_link,
        ["link_id", "from_node_id", "to_node_id", "travel_time", "capacity", "type", "description"],
        INPUT_LINK.name,
    )
    _require_columns(df_node, ["node_id", "type", "belong_to_line_id"], INPUT_NODE.name)
    _require_columns(
        df_path,
        ["path_id", "from_node_id", "to_node_id", "node_list", "link_list", "free_travel_time"],
        INPUT_PATH.name,
    )
    _require_columns(
        df_demand,
        [
            "od_id", "origion", "destination", "demand",
            "departure_time_earliest", "departure_time_latest", "path_id_list",
            "arrival_time_earliest", "arrival_time_latest",
        ],
        INPUT_DEMAND.name,
    )
    _require_columns(
        df_train,
        [
            "train_id", "line_id", "node_sequence", "link_sequence",
            "time_sequence", "capacity",
        ],
        INPUT_TRAIN_TIMETABLE.name,
    )

    nodes: Dict[int, NodeData] = {}
    for _, row in df_node.iterrows():
        bench.check()
        nid = int(row["node_id"])
        raw_line = row["belong_to_line_id"]
        line_id = _parse_optional_int(raw_line, field_name="belong_to_line_id")
        nodes[nid] = NodeData(
            node_id=nid,
            node_type=int(row["type"]),
            line_id=line_id,
        )

    links: Dict[int, LinkData] = {}
    for _, row in df_link.iterrows():
        bench.check()
        lid = int(row["link_id"])
        if lid in links:
            raise ValueError(f"Duplicate link_id={lid}")
        link = LinkData(
            link_id=lid,
            from_node_id=int(row["from_node_id"]),
            to_node_id=int(row["to_node_id"]),
            travel_time=float(row["travel_time"]),
            capacity=float(row["capacity"]),
            link_type=int(row["type"]),
            description=str(row["description"]),
        )
        if link.from_node_id not in nodes or link.to_node_id not in nodes:
            raise ValueError(f"Link {lid} references a node missing from input_node.csv")
        links[lid] = link

    paths: Dict[int, PhysicalPath] = {}
    for _, row in df_path.iterrows():
        bench.check()
        pid = int(row["path_id"])
        nseq = _parse_int_list(row["node_list"])
        lseq = _parse_int_list(row["link_list"])
        if len(nseq) != len(lseq) + 1:
            raise ValueError(f"Path {pid}: node_list/link_list length mismatch")
        for i, lid in enumerate(lseq):
            bench.check()
            if lid not in links:
                raise ValueError(f"Path {pid} references unknown link {lid}")
            link = links[lid]
            if (link.from_node_id, link.to_node_id) != (nseq[i], nseq[i + 1]):
                raise ValueError(f"Path {pid}: link {lid} endpoints mismatch")
        paths[pid] = PhysicalPath(
            path_id=pid,
            from_node_id=int(row["from_node_id"]),
            to_node_id=int(row["to_node_id"]),
            node_list=nseq,
            link_list=lseq,
            free_travel_time=float(row["free_travel_time"]),
        )

    demands: Dict[int, DemandData] = {}
    for _, row in df_demand.iterrows():
        bench.check()
        od = int(row["od_id"])
        q_float = float(row["demand"])
        if abs(q_float - round(q_float)) > 1e-8:
            raise ValueError(
                f"Agent-based models require integer unit-passenger demand; OD {od} has demand={q_float}"
            )
        path_ids = _parse_int_list(row["path_id_list"])
        dep_e = int(row["departure_time_earliest"])
        dep_l = int(row["departure_time_latest"])
        if dep_l < dep_e:
            raise ValueError(f"OD {od}: invalid departure-time window")
        d = DemandData(
            od_id=od,
            origin=int(row["origion"]),
            destination=int(row["destination"]),
            demand=int(round(q_float)),
            departure_time_earliest=dep_e,
            departure_time_latest=dep_l,
            path_id_list=path_ids,
            arrival_time_earliest=float(row["arrival_time_earliest"]),
            arrival_time_latest=float(row["arrival_time_latest"]),
        )
        for pid in path_ids:
            bench.check()
            if pid not in paths:
                raise ValueError(f"OD {od} references unknown path {pid}")
            p = paths[pid]
            if p.from_node_id != d.origin or p.to_node_id != d.destination:
                raise ValueError(f"OD {od} and path {pid} endpoints are inconsistent")
        demands[od] = d

    trains: List[TrainService] = []
    resource_capacity: Dict[Resource, float] = {}
    train_events: List[TrainEvent] = []
    event_by_resource: Dict[Resource, TrainEvent] = {}
    events_by_platform: Dict[int, List[TrainEvent]] = defaultdict(list)

    for _, row in df_train.iterrows():
        bench.check()
        train_id = int(row["train_id"])
        line_id = int(row["line_id"])
        nseq = _parse_int_list(row["node_sequence"])
        lseq = _parse_int_list(row["link_sequence"])
        tseq = _parse_float_list(row["time_sequence"])
        cap = float(row["capacity"])

        if len(nseq) != len(lseq) + 1:
            raise ValueError(f"Train {train_id}: node/link sequence length mismatch")
        if len(tseq) != len(nseq):
            raise ValueError(f"Train {train_id}: one timetable time per node is required")
        if any(tseq[i + 1] < tseq[i] - TIME_TOL for i in range(len(tseq) - 1)):
            raise ValueError(f"Train {train_id}: non-monotone time_sequence")

        train = TrainService(
            train_id=train_id,
            line_id=line_id,
            node_sequence=nseq,
            link_sequence=lseq,
            time_sequence=tseq,
            capacity=cap,
        )
        trains.append(train)

        for s, lid in enumerate(lseq):
            bench.check()
            if lid not in links:
                raise ValueError(f"Train {train_id}: unknown run link {lid}")
            link = links[lid]
            if link.link_type != LINK_TYPE_RUN:
                raise ValueError(f"Train {train_id}: link {lid} is not type=3 run link")
            if (link.from_node_id, link.to_node_id) != (nseq[s], nseq[s + 1]):
                raise ValueError(f"Train {train_id}: run link {lid} endpoints mismatch")

            resource = (train_id, lid)
            if resource in resource_capacity and abs(resource_capacity[resource] - cap) > CAPACITY_TOL:
                raise ValueError(f"Inconsistent train capacity for resource {resource}")
            resource_capacity[resource] = cap

            ev = TrainEvent(
                event_id=len(train_events),
                train_id=train_id,
                stop_index=s,
                platform_node=nseq[s],
                departure_time=float(tseq[s]),
                outgoing_resource=resource,
                outgoing_link_id=lid,
                capacity=cap,
                line_id=line_id,
            )
            train_events.append(ev)
            event_by_resource[resource] = ev
            events_by_platform[ev.platform_node].append(ev)

    for n in events_by_platform:
        bench.check()
        events_by_platform[n].sort(key=lambda e: (e.departure_time, e.train_id, e.stop_index))

    return InputData(
        nodes=nodes,
        links=links,
        paths=paths,
        demands=demands,
        trains=trains,
        resource_capacity=resource_capacity,
        train_events=train_events,
        event_by_resource=event_by_resource,
        events_by_platform=dict(events_by_platform),
    )


# =============================================================================
# 3. REALIZED-PATH GENERATION
# =============================================================================


def _find_contiguous_subsequence(sequence: Sequence[int], target: Sequence[int]) -> List[int]:
    m = len(target)
    if m == 0 or m > len(sequence):
        return []
    target_tuple = tuple(target)
    return [
        i for i in range(len(sequence) - m + 1)
        if tuple(sequence[i:i + m]) == target_tuple
    ]


@bench.timed('trajectory_enumeration')
def build_ride_option_cache(data: InputData) -> Dict[Tuple[int, ...], Tuple[RideOption, ...]]:
    run_blocks: Set[Tuple[int, ...]] = set()

    for path in data.paths.values():
        bench.check()
        i = 0
        while i < len(path.link_list):
            bench.check()
            if data.links[path.link_list[i]].link_type != LINK_TYPE_RUN:
                i += 1
                continue
            j = i
            block: List[int] = []
            while j < len(path.link_list) and data.links[path.link_list[j]].link_type == LINK_TYPE_RUN:
                bench.check()
                block.append(path.link_list[j])
                j += 1
            run_blocks.add(tuple(block))
            i = j

    cache: Dict[Tuple[int, ...], Tuple[RideOption, ...]] = {}
    for block in sorted(run_blocks):
        bench.check()
        opts: List[RideOption] = []
        m = len(block)
        for train in data.trains:
            bench.check()
            for start in _find_contiguous_subsequence(train.link_sequence, block):
                bench.check()
                resources = tuple((train.train_id, lid) for lid in block)
                opts.append(
                    RideOption(
                        train_id=train.train_id,
                        block_links=block,
                        departure_time=float(train.time_sequence[start]),
                        arrival_time=float(train.time_sequence[start + m]),
                        node_times=tuple(float(v) for v in train.time_sequence[start:start + m + 1]),
                        resources=resources,
                        first_resource=resources[0],
                    )
                )
        opts.sort(key=lambda o: (o.departure_time, o.train_id, o.arrival_time))
        if not opts:
            raise RuntimeError(f"No train covers physical run block {block}")
        cache[block] = tuple(opts)
    return cache


def _append_waiting_trace(
    node_seq: List[int],
    link_seq: List[int],
    time_seq: List[float],
    platform_node: int,
    queue_entry_link_id: int,
    ready_time: float,
    board_time: float,
) -> None:
    """Match the existing Flow-Agent output convention.

    During a one-minute waiting step, the platform node is repeated and the
    access/transfer link through which the passenger entered that queue is
    repeated in link_sequence.
    """
    if board_time <= ready_time + TIME_TOL:
        return
    r0 = _time_to_grid(ready_time)
    r1 = _time_to_grid(board_time)
    for g in range(r0 + 1, r1 + 1):
        bench.check()
        node_seq.append(platform_node)
        link_seq.append(queue_entry_link_id)
        time_seq.append(g * TIME_GRID)


@bench.timed('trajectory_enumeration')
def generate_realized_paths(
    data: InputData,
) -> Tuple[List[RealizedPathColumn], Dict[Tuple[int, ...], Tuple[RideOption, ...]]]:
    ride_cache = build_ride_option_cache(data)
    columns: List[RealizedPathColumn] = []
    seen = set()

    def add_column(
        d: DemandData,
        p: PhysicalPath,
        dep_time: float,
        final_time: float,
        trains: Tuple[int, ...],
        resources: Tuple[Resource, ...],
        episodes: Tuple[BoardingEpisode, ...],
        node_seq: Tuple[int, ...],
        link_seq: Tuple[int, ...],
        time_seq: Tuple[float, ...],
    ) -> None:
        tt = final_time - dep_time
        early = max(d.arrival_time_earliest - final_time, 0.0)
        late = max(final_time - d.arrival_time_latest, 0.0)
        sched = BETA * early + GAMMA * late
        cost = THETA * tt + sched
        key = (d.od_id, p.path_id, dep_time, trains, node_seq, link_seq, time_seq)
        if key in seen:
            return
        seen.add(key)
        columns.append(
            RealizedPathColumn(
                column_id=len(columns),
                od_id=d.od_id,
                path_id=p.path_id,
                passenger_departure_time=float(dep_time),
                final_arrival_time=float(final_time),
                total_travel_time=float(tt),
                early_time=float(early),
                late_time=float(late),
                unit_schedule_cost=float(sched),
                unit_cost=float(cost),
                train_sequence=trains,
                resources=resources,
                boarding_episodes=episodes,
                node_sequence=node_seq,
                link_sequence=link_seq,
                time_sequence=time_seq,
            )
        )

    for d in sorted(data.demands.values(), key=lambda x: x.od_id):
        bench.check()
        for pid in d.path_id_list:
            bench.check()
            p = data.paths[pid]
            for dep in range(d.departure_time_earliest, d.departure_time_latest + 1):
                bench.check()
                start_nodes = [p.from_node_id]
                start_links: List[int] = []
                start_times = [float(dep)]

                def dfs(
                    pos: int,
                    current_time: float,
                    train_seq: Tuple[int, ...],
                    resources: Tuple[Resource, ...],
                    episodes: Tuple[BoardingEpisode, ...],
                    node_seq: List[int],
                    link_seq: List[int],
                    time_seq: List[float],
                    last_queue_entry_link: Optional[int],
                ) -> None:
                    if pos == len(p.link_list):
                        if node_seq[-1] != p.to_node_id:
                            raise RuntimeError("Realized path did not end at OD destination")
                        add_column(
                            d=d,
                            p=p,
                            dep_time=float(dep),
                            final_time=float(current_time),
                            trains=train_seq,
                            resources=resources,
                            episodes=episodes,
                            node_seq=tuple(node_seq),
                            link_seq=tuple(link_seq),
                            time_seq=tuple(time_seq),
                        )
                        return

                    lid = p.link_list[pos]
                    link = data.links[lid]

                    if link.link_type != LINK_TYPE_RUN:
                        nt = current_time + link.travel_time
                        n_nodes = list(node_seq)
                        n_links = list(link_seq)
                        n_times = list(time_seq)
                        n_nodes.append(link.to_node_id)
                        n_links.append(lid)
                        n_times.append(float(nt))
                        new_queue_link = lid if link.link_type in (LINK_TYPE_ACCESS, LINK_TYPE_TRANSFER) else last_queue_entry_link
                        dfs(
                            pos + 1,
                            nt,
                            train_seq,
                            resources,
                            episodes,
                            n_nodes,
                            n_links,
                            n_times,
                            new_queue_link,
                        )
                        return

                    end = pos
                    block: List[int] = []
                    while end < len(p.link_list) and data.links[p.link_list[end]].link_type == LINK_TYPE_RUN:
                        bench.check()
                        block.append(p.link_list[end])
                        end += 1
                    block_t = tuple(block)
                    platform = data.links[block_t[0]].from_node_id
                    if last_queue_entry_link is None:
                        raise RuntimeError(
                            f"Run block {block_t} starts at platform {platform} without a preceding access/transfer queue-entry link"
                        )

                    for opt in ride_cache[block_t]:
                        bench.check()
                        if opt.departure_time + TIME_TOL < current_time:
                            continue

                        n_nodes = list(node_seq)
                        n_links = list(link_seq)
                        n_times = list(time_seq)

                        _append_waiting_trace(
                            n_nodes,
                            n_links,
                            n_times,
                            platform_node=platform,
                            queue_entry_link_id=last_queue_entry_link,
                            ready_time=current_time,
                            board_time=opt.departure_time,
                        )

                        # Traverse every run link in the block on the SAME train.
                        for r, run_lid in enumerate(block_t):
                            bench.check()
                            run_link = data.links[run_lid]
                            n_nodes.append(run_link.to_node_id)
                            n_links.append(run_lid)
                            n_times.append(float(opt.node_times[r + 1]))

                        ep = BoardingEpisode(
                            platform_node=platform,
                            queue_entry_link_id=last_queue_entry_link,
                            block_links=block_t,
                            ready_time=float(current_time),
                            selected_train_id=opt.train_id,
                            selected_departure_time=float(opt.departure_time),
                            selected_arrival_time=float(opt.arrival_time),
                            first_resource=opt.first_resource,
                        )
                        dfs(
                            end,
                            opt.arrival_time,
                            train_seq + (opt.train_id,),
                            resources + opt.resources,
                            episodes + (ep,),
                            n_nodes,
                            n_links,
                            n_times,
                            None,
                        )

                dfs(
                    pos=0,
                    current_time=float(dep),
                    train_seq=tuple(),
                    resources=tuple(),
                    episodes=tuple(),
                    node_seq=start_nodes,
                    link_seq=start_links,
                    time_seq=start_times,
                    last_queue_entry_link=None,
                )

    # Stable IDs.
    columns = [
        RealizedPathColumn(
            column_id=i,
            od_id=c.od_id,
            path_id=c.path_id,
            passenger_departure_time=c.passenger_departure_time,
            final_arrival_time=c.final_arrival_time,
            total_travel_time=c.total_travel_time,
            early_time=c.early_time,
            late_time=c.late_time,
            unit_schedule_cost=c.unit_schedule_cost,
            unit_cost=c.unit_cost,
            train_sequence=c.train_sequence,
            resources=c.resources,
            boarding_episodes=c.boarding_episodes,
            node_sequence=c.node_sequence,
            link_sequence=c.link_sequence,
            time_sequence=c.time_sequence,
        )
        for i, c in enumerate(columns)
    ]

    by_od = defaultdict(int)
    for c in columns:
        bench.check()
        by_od[c.od_id] += 1
    missing = [od for od in data.demands if by_od[od] == 0]
    if missing:
        raise RuntimeError(f"No timetable-feasible realized path for ODs {missing}")

    # A column may join a given line-direction platform queue at most once.
    for c in columns:
        bench.check()
        plats = [ep.platform_node for ep in c.boarding_episodes]
        if len(plats) != len(set(plats)):
            raise RuntimeError(
                f"Column {c.column_id} visits the same platform queue more than once. "
                "The compact FIFO frontier in this script assumes simple physical paths without repeated queue visits."
            )

    return columns, ride_cache


# =============================================================================
# 4. AGENTS AND MODEL INDEX SETS
# =============================================================================


@bench.timed('agent_generation')
def build_agents(data: InputData) -> List[AgentData]:
    agents: List[AgentData] = []
    aid = 1
    for od in sorted(data.demands):
        bench.check()
        for _ in range(data.demands[od].demand):
            bench.check()
            agents.append(AgentData(agent_id=aid, od_id=od, rank=aid))
            aid += 1
    return agents


@bench.timed('model_indexes')
def build_model_indexes(
    data: InputData,
    columns: List[RealizedPathColumn],
    ride_cache: Dict[Tuple[int, ...], Tuple[RideOption, ...]],
):
    cols_by_od: Dict[int, List[int]] = defaultdict(list)
    cols_by_resource: Dict[Resource, List[int]] = defaultdict(list)
    episode_by_col_platform: Dict[Tuple[int, int], BoardingEpisode] = {}
    cols_by_od_platform: Dict[Tuple[int, int], List[int]] = defaultdict(list)

    for c in columns:
        bench.check()
        cols_by_od[c.od_id].append(c.column_id)
        for res in c.resources:
            bench.check()
            cols_by_resource[res].append(c.column_id)
        for ep in c.boarding_episodes:
            bench.check()
            episode_by_col_platform[(c.column_id, ep.platform_node)] = ep
            cols_by_od_platform[(c.od_id, ep.platform_node)].append(c.column_id)

    # Map every train event to its order within that unified platform queue.
    event_rank_by_resource: Dict[Resource, int] = {}
    event_by_platform_rank: Dict[Tuple[int, int], TrainEvent] = {}
    for n, events in data.events_by_platform.items():
        bench.check()
        for rank, ev in enumerate(events):
            bench.check()
            event_rank_by_resource[ev.outgoing_resource] = rank
            event_by_platform_rank[(n, rank)] = ev

    # Determine which realized-path columns SKIP which earlier eligible train.
    # This is the key index for spatial FIFO/work-conserving boarding.
    skip_cols_by_event: Dict[int, Set[int]] = defaultdict(set)

    for c in columns:
        bench.check()
        for ep in c.boarding_episodes:
            bench.check()
            selected_rank = event_rank_by_resource[ep.first_resource]
            for opt in ride_cache[ep.block_links]:
                bench.check()
                if opt.departure_time + TIME_TOL < ep.ready_time:
                    continue
                if opt.first_resource not in event_rank_by_resource:
                    continue
                ev = data.event_by_resource[opt.first_resource]
                if ev.platform_node != ep.platform_node:
                    continue
                opt_rank = event_rank_by_resource[opt.first_resource]
                if opt_rank < selected_rank:
                    skip_cols_by_event[ev.event_id].add(c.column_id)

    # Compatibility audit for the compact unified-platform temporal FIFO.
    # For the supplied instance, every platform has one next run block. If a
    # future case contains several distinct service-compatibility blocks from
    # the same physical platform, the queue model should be extended carefully.
    blocks_by_platform: Dict[int, Set[Tuple[int, ...]]] = defaultdict(set)
    for c in columns:
        bench.check()
        for ep in c.boarding_episodes:
            bench.check()
            blocks_by_platform[ep.platform_node].add(ep.block_links)

    return {
        "cols_by_od": dict(cols_by_od),
        "cols_by_resource": dict(cols_by_resource),
        "episode_by_col_platform": episode_by_col_platform,
        "cols_by_od_platform": dict(cols_by_od_platform),
        "event_rank_by_resource": event_rank_by_resource,
        "event_by_platform_rank": event_by_platform_rank,
        "skip_cols_by_event": {k: set(v) for k, v in skip_cols_by_event.items()},
        "blocks_by_platform": {k: set(v) for k, v in blocks_by_platform.items()},
    }


# =============================================================================
# 5. GUROBI MODEL
# =============================================================================


def _status_name(GRB, status: int) -> str:
    names = {
        GRB.OPTIMAL: "OPTIMAL",
        GRB.TIME_LIMIT: "TIME_LIMIT",
        GRB.INFEASIBLE: "INFEASIBLE",
        GRB.INF_OR_UNBD: "INF_OR_UNBD",
        GRB.UNBOUNDED: "UNBOUNDED",
        GRB.INTERRUPTED: "INTERRUPTED",
        GRB.SUBOPTIMAL: "SUBOPTIMAL",
    }
    return names.get(status, str(status))


def _safe_write_gurobi_model(model, output_dir: Path, filename: str) -> None:
    """Best-effort Gurobi model export that is safe on Chinese Windows paths.

    Gurobi 10.x on Windows can fail when ``Model.write`` receives a full path
    containing non-ASCII characters.  We therefore:
      1) try the requested full path;
      2) if that fails, temporarily chdir to the output directory and pass only
         the ASCII file name to Gurobi;
      3) if export still fails, print a warning and continue optimization.

    The .lp file is diagnostic only and has no effect on the mathematical
    model or the solver result.
    """
    if not WRITE_GUROBI_LP_FILES:
        return

    output_dir.mkdir(parents=True, exist_ok=True)
    target = output_dir / filename

    try:
        model.write(str(target))
        return
    except Exception as exc_full:
        print(
            f"[WARN] Gurobi could not write LP using the full path: {target}\n"
            f"       {type(exc_full).__name__}: {exc_full}\n"
            "       Retrying with an ASCII-only relative filename."
        )

    old_cwd = Path.cwd()
    try:
        os.chdir(output_dir)
        model.write(filename)
        print(f"[INFO] Gurobi model written via relative path: {target}")
    except Exception as exc_rel:
        print(
            f"[WARN] Gurobi model export skipped; optimization will continue.\n"
            f"       {type(exc_rel).__name__}: {exc_rel}"
        )
    finally:
        os.chdir(old_cwd)


@bench.timed('model_build')
def solve_model(
    data: InputData,
    columns: List[RealizedPathColumn],
    agents: List[AgentData],
    indexes,
    model_name: str,
    temporal_fifo: bool,
    spatial_fifo: bool,
    output_dir: Path,
    complete_started: Optional[float] = None,
) -> ModelSolution:
    try:
        import gurobipy as gp
        from gurobipy import GRB
    except ImportError as exc:
        raise RuntimeError(
            "gurobipy is not installed/configured in this Python environment. "
            "Install Gurobi and its Python package, then rerun the script."
        ) from exc

    t_wall0 = time.perf_counter() if complete_started is None else complete_started
    output_dir.mkdir(parents=True, exist_ok=True)

    model = gp.Model(model_name)
    model.Params.OutputFlag = GUROBI_OUTPUT_FLAG
    model.Params.Threads = GUROBI_THREADS
    if GUROBI_TIME_LIMIT is not None:
        model.Params.TimeLimit = GUROBI_TIME_LIMIT
    model.Params.MIPGap = GUROBI_MIP_GAP
    model.Params.MIPFocus = GUROBI_MIP_FOCUS
    model.Params.Heuristics = GUROBI_HEURISTICS
    model.Params.Symmetry = GUROBI_SYMMETRY
    model.Params.Presolve = GUROBI_PRESOLVE
    model.Params.Seed = GUROBI_SEED

    cols_by_od = indexes["cols_by_od"]
    cols_by_resource = indexes["cols_by_resource"]
    episode_by_col_platform = indexes["episode_by_col_platform"]
    cols_by_od_platform = indexes["cols_by_od_platform"]
    event_rank_by_resource = indexes["event_rank_by_resource"]
    skip_cols_by_event = indexes["skip_cols_by_event"]

    agents_by_od: Dict[int, List[AgentData]] = defaultdict(list)
    for a in agents:
        bench.check()
        agents_by_od[a.od_id].append(a)

    # -------------------------------------------------------------------------
    # Agent-choice variables z[a,j].
    # -------------------------------------------------------------------------
    z = {}
    for a in agents:
        bench.check()
        for j in cols_by_od[a.od_id]:
            bench.check()
            z[a.agent_id, j] = model.addVar(
                vtype=GRB.BINARY,
                name=f"z[{a.agent_id},{j}]",
            )
    model.update()

    # Each agent selects exactly one complete train-specific trajectory.
    for a in agents:
        bench.check()
        model.addConstr(
            gp.quicksum(z[a.agent_id, j] for j in cols_by_od[a.od_id]) == 1,
            name=f"agent_choice[{a.agent_id}]",
        )

    # -------------------------------------------------------------------------
    # Train-segment load expressions and hard capacity.
    # -------------------------------------------------------------------------
    resource_load_expr = {}
    for res, cap in sorted(data.resource_capacity.items()):
        bench.check()
        terms = []
        relevant_cols = set(cols_by_resource.get(res, []))
        if relevant_cols:
            for od, od_agents in agents_by_od.items():
                bench.check()
                js = [j for j in cols_by_od[od] if j in relevant_cols]
                if not js:
                    continue
                for a in od_agents:
                    bench.check()
                    terms.extend(z[a.agent_id, j] for j in js)
        expr = gp.quicksum(terms) if terms else gp.LinExpr(0.0)
        resource_load_expr[res] = expr
        model.addConstr(expr <= cap, name=f"capacity[{res[0]},{res[1]}]")

    # -------------------------------------------------------------------------
    # Train occupancy / residual-capacity variables.
    # These make the space-FIFO interpretation explicit and provide outputs
    # aligned with Occ^- -> ResidualCap -> Board -> Occ^+.
    # -------------------------------------------------------------------------
    occ_minus = {}
    residual_cap = {}
    board_var = {}
    occ_plus = {}

    train_by_id = {tr.train_id: tr for tr in data.trains}
    for ev in data.train_events:
        bench.check()
        k = ev.train_id
        s = ev.stop_index
        tr = train_by_id[k]
        curr_res = ev.outgoing_resource
        prev_res = None if s == 0 else (k, tr.link_sequence[s - 1])

        curr_cols = set(cols_by_resource.get(curr_res, []))
        prev_cols = set(cols_by_resource.get(prev_res, [])) if prev_res is not None else set()
        through_cols = curr_cols & prev_cols
        board_cols = curr_cols - prev_cols

        through_terms = []
        board_terms = []
        for od, od_agents in agents_by_od.items():
            bench.check()
            through_js = [j for j in cols_by_od[od] if j in through_cols]
            board_js = [j for j in cols_by_od[od] if j in board_cols]
            for a in od_agents:
                bench.check()
                through_terms.extend(z[a.agent_id, j] for j in through_js)
                board_terms.extend(z[a.agent_id, j] for j in board_js)

        through_expr = gp.quicksum(through_terms) if through_terms else gp.LinExpr(0.0)
        new_board_expr = gp.quicksum(board_terms) if board_terms else gp.LinExpr(0.0)

        occ_minus[ev.event_id] = model.addVar(lb=0.0, ub=ev.capacity, vtype=GRB.CONTINUOUS,
                                               name=f"occ_minus[{ev.event_id}]")
        residual_cap[ev.event_id] = model.addVar(lb=0.0, ub=ev.capacity, vtype=GRB.CONTINUOUS,
                                                  name=f"residual_cap[{ev.event_id}]")
        board_var[ev.event_id] = model.addVar(lb=0.0, ub=ev.capacity, vtype=GRB.CONTINUOUS,
                                               name=f"board[{ev.event_id}]")
        occ_plus[ev.event_id] = model.addVar(lb=0.0, ub=ev.capacity, vtype=GRB.CONTINUOUS,
                                              name=f"occ_plus[{ev.event_id}]")

        model.addConstr(occ_minus[ev.event_id] == through_expr,
                        name=f"occ_minus_def[{ev.event_id}]")
        model.addConstr(residual_cap[ev.event_id] == ev.capacity - occ_minus[ev.event_id],
                        name=f"residual_def[{ev.event_id}]")
        model.addConstr(board_var[ev.event_id] == new_board_expr,
                        name=f"board_def[{ev.event_id}]")
        model.addConstr(board_var[ev.event_id] <= residual_cap[ev.event_id],
                        name=f"board_residual[{ev.event_id}]")
        model.addConstr(occ_plus[ev.event_id] == occ_minus[ev.event_id] + board_var[ev.event_id],
                        name=f"occ_plus_def[{ev.event_id}]")
        model.addConstr(occ_plus[ev.event_id] == resource_load_expr[curr_res],
                        name=f"occ_plus_resource_match[{ev.event_id}]")

    # -------------------------------------------------------------------------
    # Spatial FIFO / work-conserving boarding.
    # If any selected trajectory skips an earlier eligible train event, that
    # skipped train must leave full. Temporal order is NOT used here.
    # -------------------------------------------------------------------------
    omega = {}
    if spatial_fifo:
        for ev in data.train_events:
            bench.check()
            skipped_cols = skip_cols_by_event.get(ev.event_id, set())
            if not skipped_cols:
                continue

            wait_terms = []
            potentially_waiting_agents: Set[int] = set()
            for od, od_agents in agents_by_od.items():
                bench.check()
                js = [j for j in cols_by_od[od] if j in skipped_cols]
                if not js:
                    continue
                for a in od_agents:
                    bench.check()
                    potentially_waiting_agents.add(a.agent_id)
                    wait_terms.extend(z[a.agent_id, j] for j in js)

            if not wait_terms:
                continue
            wait_expr = gp.quicksum(wait_terms)
            m_wait = max(1, len(potentially_waiting_agents))

            omega[ev.event_id] = model.addVar(vtype=GRB.BINARY,
                                               name=f"residual_queue[{ev.event_id}]")
            # wait_expr > 0 iff omega = 1 (wait_expr is integer under binary z).
            model.addConstr(wait_expr <= m_wait * omega[ev.event_id],
                            name=f"spatial_fifo_wait_upper[{ev.event_id}]")
            model.addConstr(omega[ev.event_id] <= wait_expr,
                            name=f"spatial_fifo_wait_lower[{ev.event_id}]")
            # A positive residual queue implies zero residual train capacity.
            model.addConstr(
                ev.capacity - occ_plus[ev.event_id]
                <= ev.capacity * (1 - omega[ev.event_id]),
                name=f"spatial_fifo_full_if_wait[{ev.event_id}]",
            )

    # -------------------------------------------------------------------------
    # Temporal FIFO / unified platform queue frontier.
    # Entry and transfer passengers entering the same line-direction platform n
    # are pooled into ONE queue. No passenger-pair precedence binaries are used.
    # -------------------------------------------------------------------------
    frontier = {}
    fifo_constraint_count = 0
    if temporal_fifo:
        n_agents = len(agents)
        R = n_agents + 1

        # Maximum key for safe Big-M.
        max_ready_grid = 0
        for c in columns:
            bench.check()
            for ep in c.boarding_episodes:
                bench.check()
                max_ready_grid = max(max_ready_grid, _time_to_grid(ep.ready_time))
        max_theta = R * max_ready_grid + max(a.rank for a in agents)
        M_theta = max_theta + R + n_agents + 10

        # For each platform, enforce prefix consistency only at event ranks that
        # can separate some candidate boardings from later candidate boardings.
        for n, events in sorted(data.events_by_platform.items()):
            # Which ODs can visit this platform?
            bench.check()
            ods_here = sorted({od for (od, nn) in cols_by_od_platform if nn == n})
            if not ods_here:
                continue

            # Distinct selected event ranks available in the realized-path pool.
            possible_ranks: Set[int] = set()
            for od in ods_here:
                bench.check()
                for j in cols_by_od_platform.get((od, n), []):
                    bench.check()
                    ep = episode_by_col_platform[j, n]
                    possible_ranks.add(event_rank_by_resource[ep.first_resource])
            sorted_ranks = sorted(possible_ranks)
            if len(sorted_ranks) <= 1:
                continue
            relevant_ranks = sorted_ranks[:-1]  # after final possible boarding everyone has boarded

            prev_phi = None
            for erank in relevant_ranks:
                bench.check()
                ev = events[erank]
                phi = model.addVar(lb=0.0, ub=float(max_theta), vtype=GRB.CONTINUOUS,
                                   name=f"fifo_frontier[{n},{erank}]")
                frontier[n, erank] = phi
                if prev_phi is not None:
                    model.addConstr(prev_phi <= phi,
                                    name=f"fifo_frontier_monotone[{n},{erank}]")
                prev_phi = phi

                for od in ods_here:
                    bench.check()
                    potential_cols = list(dict.fromkeys(cols_by_od_platform.get((od, n), [])))
                    if not potential_cols:
                        continue
                    for a in agents_by_od[od]:
                        bench.check()
                        visit_expr = gp.quicksum(z[a.agent_id, j] for j in potential_cols)

                        theta_terms = []
                        boarded_terms = []
                        for j in potential_cols:
                            bench.check()
                            ep = episode_by_col_platform[j, n]
                            ready_grid = _time_to_grid(ep.ready_time)
                            theta_key = R * ready_grid + a.rank
                            theta_terms.append(theta_key * z[a.agent_id, j])
                            selected_rank = event_rank_by_resource[ep.first_resource]
                            if selected_rank <= erank:
                                boarded_terms.append(z[a.agent_id, j])

                        theta_expr = gp.quicksum(theta_terms)
                        y_expr = gp.quicksum(boarded_terms) if boarded_terms else gp.LinExpr(0.0)

                        # If boarded by this event: theta <= phi.
                        model.addConstr(
                            theta_expr <= phi + M_theta * (1 - y_expr) + M_theta * (1 - visit_expr),
                            name=f"time_fifo_served[{n},{erank},{a.agent_id}]",
                        )
                        # If still unboarded: theta >= phi + 1.
                        model.addConstr(
                            theta_expr >= phi + 1 - M_theta * y_expr - M_theta * (1 - visit_expr),
                            name=f"time_fifo_waiting[{n},{erank},{a.agent_id}]",
                        )
                        fifo_constraint_count += 2

    # Objective.
    model.setObjective(
        gp.quicksum(
            columns[j].unit_cost * z[a.agent_id, j]
            for a in agents
            for j in cols_by_od[a.od_id]
        ),
        GRB.MINIMIZE,
    )

    model.update()
    bench.save_json(bench.output/'model_build_summary.json', dict(variables=model.NumVars, binary_variables=model.NumBinVars, constraints=model.NumConstrs, nonzeros=model.NumNZs, realized_path_columns=len(columns), build_elapsed_seconds=__import__('time').perf_counter()-bench.started))
    _safe_write_gurobi_model(model, output_dir, f"{model_name}.lp")
    bench.apply_mip_start(model, z, columns, agents)
    bench.optimize(model)
    bench.finish_computation()

    status = _status_name(GRB, model.Status)
    has_incumbent = model.SolCount > 0
    is_opt = model.Status == GRB.OPTIMAL
    selected: Dict[int, int] = {}

    if has_incumbent:
        for a in agents:
            bench.check()
            vals = [(j, float(z[a.agent_id, j].X)) for j in cols_by_od[a.od_id]]
            j_best, v_best = max(vals, key=lambda x: x[1])
            if v_best < 0.5:
                raise RuntimeError(
                    f"Model {model_name}: no binary selected column found for agent {a.agent_id}; max z={v_best}"
                )
            selected[a.agent_id] = j_best

    wall = time.perf_counter() - t_wall0
    incumbent = float(model.ObjVal) if has_incumbent else None
    best_bound = float(model.ObjBound) if hasattr(model, "ObjBound") else None
    mip_gap = float(model.MIPGap) if has_incumbent else None

    sol = ModelSolution(
        model_name=model_name,
        status_name=status,
        is_optimal=is_opt,
        has_incumbent=has_incumbent,
        incumbent_objective=incumbent,
        best_bound=best_bound,
        mip_gap=mip_gap,
        gurobi_runtime=float(model.Runtime),
        complete_wall_time=float(wall),
        selected_column_by_agent=selected,
        variable_count=int(model.NumVars),
        binary_count=int(model.NumBinVars),
        constraint_count=int(model.NumConstrs),
    )

    # Write incumbent outputs whenever available.
    if has_incumbent:
        write_model_outputs(
            data=data,
            columns=columns,
            agents=agents,
            indexes=indexes,
            solution=sol,
            temporal_fifo_required=temporal_fifo,
            spatial_fifo_required=spatial_fifo,
            output_dir=output_dir,
        )
    else:
        pd.DataFrame([
            {
                "model": model_name,
                "status": status,
                "has_incumbent": 0,
                "is_optimal": int(is_opt),
                "best_bound": best_bound,
                "gurobi_runtime_seconds": model.Runtime,
                "complete_wall_time_seconds": wall,
                "variables": model.NumVars,
                "binary_variables": model.NumBinVars,
                "constraints": model.NumConstrs,
            }
        ]).to_csv(output_dir / "model_summary.csv", index=False, encoding="utf-8-sig")

    from pmc_output_adapter import finalize_outputs
    finalize_outputs(data, columns, agents, indexes, sol, temporal_fifo, spatial_fifo,
                     output_dir, t_wall0, dict(travel_time=THETA, early=BETA, late=GAMMA))
    return sol


# =============================================================================
# 6. OUTPUTS AND INDEPENDENT SOLUTION AUDIT
# =============================================================================


def _fmt_time(v: float) -> str:
    if abs(v - round(v)) <= 1e-9:
        return str(int(round(v)))
    return f"{v:.10g}"


def _seq_int_text(seq: Sequence[int]) -> str:
    return ";".join(str(v) for v in seq)


def _seq_time_text(seq: Sequence[float]) -> str:
    return ";".join(_fmt_time(v) for v in seq)


def selected_resource_loads(
    columns: List[RealizedPathColumn],
    selected: Dict[int, int],
) -> Dict[Resource, int]:
    load = defaultdict(int)
    for _, j in selected.items():
        bench.check()
        for res in columns[j].resources:
            bench.check()
            load[res] += 1
    return dict(load)


def independent_audit(
    data: InputData,
    columns: List[RealizedPathColumn],
    agents: List[AgentData],
    indexes,
    selected: Dict[int, int],
    temporal_fifo_required: bool,
    spatial_fifo_required: bool,
    output_dir: Path,
):
    loads = selected_resource_loads(columns, selected)

    # ----- Capacity audit -----
    cap_rows = []
    cap_viol = 0
    for res, cap in sorted(data.resource_capacity.items()):
        bench.check()
        ld = loads.get(res, 0)
        viol = max(ld - cap, 0.0)
        if viol > CAPACITY_TOL:
            cap_viol += 1
        cap_rows.append({
            "train_id": res[0],
            "link_id": res[1],
            "load": ld,
            "capacity": cap,
            "remaining_capacity": cap - ld,
            "violation": viol,
        })
    pd.DataFrame(cap_rows).to_csv(output_dir / "capacity_audit.csv", index=False, encoding="utf-8-sig")

    # ----- Spatial FIFO audit -----
    event_rank_by_resource = indexes["event_rank_by_resource"]
    ride_cache_local = build_ride_option_cache(data)
    spatial_rows = []
    spatial_viol = 0
    for a in agents:
        bench.check()
        c = columns[selected[a.agent_id]]
        for ep_idx, ep in enumerate(c.boarding_episodes):
            bench.check()
            selected_rank = event_rank_by_resource[ep.first_resource]
            for opt in ride_cache_local[ep.block_links]:
                bench.check()
                if opt.departure_time + TIME_TOL < ep.ready_time:
                    continue
                ev = data.event_by_resource[opt.first_resource]
                if ev.platform_node != ep.platform_node:
                    continue
                r = event_rank_by_resource[opt.first_resource]
                if r >= selected_rank:
                    continue
                ld = loads.get(ev.outgoing_resource, 0)
                full = abs(ld - ev.capacity) <= CAPACITY_TOL
                if not full:
                    spatial_viol += 1
                    spatial_rows.append({
                        "agent_id": a.agent_id,
                        "column_id": c.column_id,
                        "episode_index": ep_idx,
                        "platform_node": ep.platform_node,
                        "ready_time": ep.ready_time,
                        "selected_train_id": ep.selected_train_id,
                        "selected_departure_time": ep.selected_departure_time,
                        "skipped_train_id": ev.train_id,
                        "skipped_departure_time": ev.departure_time,
                        "skipped_train_load": ld,
                        "skipped_train_capacity": ev.capacity,
                    })
    pd.DataFrame(spatial_rows, columns=['agent_id','column_id','episode_index','platform_node','ready_time','selected_train_id','selected_departure_time','skipped_train_id','skipped_departure_time','skipped_train_load','skipped_train_capacity']).to_csv(output_dir / "spatial_fifo_violations_detail.csv", index=False, encoding="utf-8-sig")

    # ----- Temporal FIFO audit -----
    episodes_by_platform: Dict[int, List[Tuple[float, int, int, int, float]]] = defaultdict(list)
    # (ready_time, agent_rank, agent_id, train_event_rank, board_time)
    for a in agents:
        bench.check()
        c = columns[selected[a.agent_id]]
        for ep in c.boarding_episodes:
            bench.check()
            episodes_by_platform[ep.platform_node].append(
                (
                    ep.ready_time,
                    a.rank,
                    a.agent_id,
                    event_rank_by_resource[ep.first_resource],
                    ep.selected_departure_time,
                )
            )

    temporal_rows = []
    temporal_viol = 0
    for n, eps in sorted(episodes_by_platform.items()):
        bench.check()
        eps.sort(key=lambda x: (x[0], x[1]))
        max_board_rank_so_far = -1
        prev = None
        for ready, rank, aid, board_rank, board_time in eps:
            bench.check()
            if board_rank < max_board_rank_so_far:
                temporal_viol += 1
                temporal_rows.append({
                    "platform_node": n,
                    "agent_id": aid,
                    "ready_time": ready,
                    "agent_rank": rank,
                    "board_event_rank": board_rank,
                    "board_time": board_time,
                    "earlier_queue_max_board_event_rank": max_board_rank_so_far,
                })
            max_board_rank_so_far = max(max_board_rank_so_far, board_rank)
            prev = (ready, rank, aid, board_rank, board_time)
    pd.DataFrame(temporal_rows, columns=['platform_node','agent_id','ready_time','agent_rank','board_event_rank','board_time','earlier_queue_max_board_event_rank']).to_csv(output_dir / "temporal_fifo_violations_detail.csv", index=False, encoding="utf-8-sig")

    # Enforce required dimensions independently of Gurobi formulation.
    if cap_viol > 0:
        raise RuntimeError(f"Independent audit found {cap_viol} train-capacity violations")
    if spatial_fifo_required and spatial_viol > 0:
        raise RuntimeError(f"Independent audit found {spatial_viol} spatial-FIFO violations")
    if temporal_fifo_required and temporal_viol > 0:
        raise RuntimeError(f"Independent audit found {temporal_viol} temporal-FIFO violations")

    return {
        "capacity_violations": cap_viol,
        "spatial_fifo_violations": spatial_viol,
        "temporal_fifo_violations": temporal_viol,
    }


def write_model_outputs(
    data: InputData,
    columns: List[RealizedPathColumn],
    agents: List[AgentData],
    indexes,
    solution: ModelSolution,
    temporal_fifo_required: bool,
    spatial_fifo_required: bool,
    output_dir: Path,
) -> None:
    selected = solution.selected_column_by_agent

    # Main trajectory output compatible with prior Flow-Agent format.
    traj_rows = []
    detail_rows = []
    for a in sorted(agents, key=lambda x: x.agent_id):
        bench.check()
        c = columns[selected[a.agent_id]]
        traj_rows.append({
            "agent_id": a.agent_id,
            "node_sequence": _seq_int_text(c.node_sequence),
            "link_sequence": _seq_int_text(c.link_sequence),
            "time_sequence": _seq_time_text(c.time_sequence),
            "early_time": c.early_time,
            "delay_time": c.late_time,
            "time_cost": c.unit_schedule_cost,
            "volume": 1,
            "total_travel_time": c.total_travel_time,
        })
        detail_rows.append({
            "agent_id": a.agent_id,
            "od_id": a.od_id,
            "agent_rank": a.rank,
            "column_id": c.column_id,
            "path_id": c.path_id,
            "passenger_departure_time": c.passenger_departure_time,
            "final_arrival_time": c.final_arrival_time,
            "unit_cost": c.unit_cost,
            "train_sequence": ";".join(str(x) for x in c.train_sequence),
            "node_sequence": _seq_int_text(c.node_sequence),
            "link_sequence": _seq_int_text(c.link_sequence),
            "time_sequence": _seq_time_text(c.time_sequence),
        })

    pd.DataFrame(traj_rows).to_csv(
        output_dir / "output_agent_reservation_timetable_optimized.csv",
        index=False,
        encoding="utf-8-sig",
    )
    pd.DataFrame(detail_rows).to_csv(
        output_dir / "optimized_agent_assignment_detailed.csv",
        index=False,
        encoding="utf-8-sig",
    )

    # Aggregate optimized agent choices back to OD-path-departure flow.
    agg = defaultdict(int)
    for a in agents:
        bench.check()
        c = columns[selected[a.agent_id]]
        agg[(c.od_id, c.path_id, c.passenger_departure_time)] += 1
    pd.DataFrame([
        {
            "od_id": od,
            "path_id": p,
            "departure_time": t,
            "flow": q,
        }
        for (od, p, t), q in sorted(agg.items())
    ]).to_csv(output_dir / "optimized_path_departure_flow.csv", index=False, encoding="utf-8-sig")

    # Boarding episodes.
    ep_rows = []
    for a in agents:
        bench.check()
        c = columns[selected[a.agent_id]]
        for h, ep in enumerate(c.boarding_episodes):
            bench.check()
            ep_rows.append({
                "agent_id": a.agent_id,
                "od_id": a.od_id,
                "path_id": c.path_id,
                "episode_index": h,
                "platform_node": ep.platform_node,
                "queue_entry_link_id": ep.queue_entry_link_id,
                "ready_time": ep.ready_time,
                "selected_train_id": ep.selected_train_id,
                "boarding_time": ep.selected_departure_time,
                "train_arrival_at_end_of_block": ep.selected_arrival_time,
                "block_links": ";".join(str(x) for x in ep.block_links),
            })
    pd.DataFrame(ep_rows).to_csv(output_dir / "optimized_boarding_episodes.csv", index=False, encoding="utf-8-sig")

    # Train occupancy / residual-capacity reconstruction.
    loads = selected_resource_loads(columns, selected)
    train_by_id = {tr.train_id: tr for tr in data.trains}
    occ_rows = []
    for ev in data.train_events:
        bench.check()
        tr = train_by_id[ev.train_id]
        curr_res = ev.outgoing_resource
        prev_res = None if ev.stop_index == 0 else (ev.train_id, tr.link_sequence[ev.stop_index - 1])

        through = 0
        new_board = 0
        alight = 0
        for a in agents:
            bench.check()
            c = columns[selected[a.agent_id]]
            has_curr = curr_res in c.resources
            has_prev = prev_res in c.resources if prev_res is not None else False
            if has_curr and has_prev:
                through += 1
            elif has_curr and not has_prev:
                new_board += 1
            elif has_prev and not has_curr:
                alight += 1

        occ_minus = through
        residual = ev.capacity - occ_minus
        occ_plus = loads.get(curr_res, 0)
        occ_rows.append({
            "event_id": ev.event_id,
            "train_id": ev.train_id,
            "stop_index": ev.stop_index,
            "platform_node": ev.platform_node,
            "departure_time": ev.departure_time,
            "outgoing_link_id": ev.outgoing_link_id,
            "alight": alight,
            "occ_minus_after_alighting": occ_minus,
            "residual_boarding_capacity": residual,
            "new_board": new_board,
            "occ_plus_after_boarding": occ_plus,
            "train_capacity": ev.capacity,
        })
    pd.DataFrame(occ_rows).to_csv(output_dir / "train_occupancy_residual_capacity.csv", index=False, encoding="utf-8-sig")

    audit = independent_audit(
        data=data,
        columns=columns,
        agents=agents,
        indexes=indexes,
        selected=selected,
        temporal_fifo_required=temporal_fifo_required,
        spatial_fifo_required=spatial_fifo_required,
        output_dir=output_dir,
    )

    # Objective reconstruction.
    obj_reconstructed = sum(columns[selected[a.agent_id]].unit_cost for a in agents)
    if solution.incumbent_objective is not None and abs(obj_reconstructed - solution.incumbent_objective) > OBJECTIVE_TOL:
        raise RuntimeError(
            f"Objective mismatch for {solution.model_name}: model={solution.incumbent_objective}, "
            f"reconstructed={obj_reconstructed}"
        )

    summary_row = {
        "model": solution.model_name,
        "status": solution.status_name,
        "is_optimal": int(solution.is_optimal),
        "has_incumbent": int(solution.has_incumbent),
        "incumbent_objective": solution.incumbent_objective,
        "best_bound": solution.best_bound,
        "mip_gap": solution.mip_gap,
        "gurobi_runtime_seconds": solution.gurobi_runtime,
        "complete_wall_time_seconds": solution.complete_wall_time,
        "variables": solution.variable_count,
        "binary_variables": solution.binary_count,
        "constraints": solution.constraint_count,
        "temporal_fifo_required": int(temporal_fifo_required),
        "spatial_fifo_required": int(spatial_fifo_required),
        "capacity_violations": audit["capacity_violations"],
        "spatial_fifo_violations": audit["spatial_fifo_violations"],
        "temporal_fifo_violations": audit["temporal_fifo_violations"],
        "objective_reconstructed": obj_reconstructed,
    }
    pd.DataFrame([summary_row]).to_csv(output_dir / "model_summary.csv", index=False, encoding="utf-8-sig")


# =============================================================================
# 7. SHARED POOL OUTPUT / STRUCTURE AUDIT
# =============================================================================


def write_shared_realized_path_pool(
    data: InputData,
    columns: List[RealizedPathColumn],
    indexes,
    agents: List[AgentData],
) -> None:
    shared_dir = OUTPUT_ROOT / "shared_realized_path_pool"
    shared_dir.mkdir(parents=True, exist_ok=True)

    rows = []
    for c in columns:
        bench.check()
        rows.append({
            "column_id": c.column_id,
            "od_id": c.od_id,
            "path_id": c.path_id,
            "passenger_departure_time": c.passenger_departure_time,
            "final_arrival_time": c.final_arrival_time,
            "total_travel_time": c.total_travel_time,
            "early_time": c.early_time,
            "late_time": c.late_time,
            "unit_schedule_cost": c.unit_schedule_cost,
            "unit_cost": c.unit_cost,
            "train_sequence": ";".join(str(x) for x in c.train_sequence),
            "resources": ";".join(f"{a}:{b}" for a, b in c.resources),
            "node_sequence": _seq_int_text(c.node_sequence),
            "link_sequence": _seq_int_text(c.link_sequence),
            "time_sequence": _seq_time_text(c.time_sequence),
        })
    pd.DataFrame(rows).to_csv(shared_dir / "all_realized_paths.csv", index=False, encoding="utf-8-sig")

    counts = defaultdict(int)
    for c in columns:
        bench.check()
        counts[c.od_id] += 1

    block_rows = []
    for n, blocks in sorted(indexes["blocks_by_platform"].items()):
        bench.check()
        block_rows.append({
            "platform_node": n,
            "number_of_distinct_next_ride_blocks": len(blocks),
            "next_ride_blocks": " | ".join(";".join(map(str, b)) for b in sorted(blocks)),
        })
    pd.DataFrame(block_rows).to_csv(shared_dir / "platform_service_compatibility_audit.csv", index=False, encoding="utf-8-sig")

    structure = [{
        "number_of_agents": len(agents),
        "number_of_ods": len(data.demands),
        "number_of_physical_paths": len(data.paths),
        "number_of_trains": len(data.trains),
        "number_of_train_events": len(data.train_events),
        "number_of_train_segment_resources": len(data.resource_capacity),
        "number_of_realized_paths": len(columns),
        "realized_paths_by_od": ";".join(f"{od}:{counts[od]}" for od in sorted(counts)),
        "number_of_platform_queues": len(indexes["blocks_by_platform"]),
        "number_of_platforms_with_multiple_service_blocks": sum(
            1 for blocks in indexes["blocks_by_platform"].values() if len(blocks) > 1
        ),
    }]
    pd.DataFrame(structure).to_csv(shared_dir / "model_structure_audit.csv", index=False, encoding="utf-8-sig")


# =============================================================================
# 8. MAIN: SOLVE ALL THREE MODELS
# =============================================================================


MODEL_SPECS = {
    'LB1': ('LB1_timeFIFO_relaxed_spatialFIFO_relaxed', False, False, 'LB1_both_FIFO_relaxed'),
    'FULL': ('FULL_timeFIFO_retained_spatialFIFO_retained', True, True, 'FULL_agent_SO'),
}


def fresh_directory(path):
    from datetime import datetime
    if not path.exists() or not any(path.iterdir()):
        path.mkdir(parents=True, exist_ok=True)
        return path
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S')
    candidate = path.with_name(path.name + '_' + stamp)
    index = 1
    while candidate.exists():
        bench.check()
        candidate = path.with_name(path.name + '_' + stamp + '_' + str(index))
        index += 1
    candidate.mkdir(parents=True)
    return candidate


def run_single(kind, output_root=None):
    if kind not in MODEL_SPECS:
        raise ValueError('Only FULL and LB1 are supported')
    started = time.perf_counter()
    name, temporal, spatial, folder = MODEL_SPECS[kind]
    parent = OUTPUT_ROOT if output_root is None else Path(output_root)
    directory = fresh_directory(parent / folder)
    data = read_input_data()
    columns, ride_cache = generate_realized_paths(data)
    agents = build_agents(data)
    indexes = build_model_indexes(data, columns, ride_cache)
    solution = solve_model(data, columns, agents, indexes, name, temporal, spatial,
                           directory, complete_started=started)
    return solution, directory


def main():
    root = fresh_directory(OUTPUT_ROOT)
    solutions, rows = [], []
    for kind in ('LB1', 'FULL'):
        bench.check()
        solution, directory = run_single(kind, root)
        solutions.append(solution)
        rows.extend(pd.read_csv(directory/'model_summary.csv').to_dict('records'))
        pd.DataFrame(rows).to_csv(root/'two_model_solution_summary.csv', index=False, encoding='utf-8-sig')
    lb, full = solutions
    if lb.is_optimal and full.is_optimal:
        if lb.incumbent_objective > full.incumbent_objective + OBJECTIVE_TOL:
            raise RuntimeError('Invalid proven-optimum hierarchy: LB1 > FULL')
    print('[DONE]', root)


if __name__ == '__main__':
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    from batch_runner import main as run_random_experiment
    sys.argv[1:1] = ['--methods', 'LB1', 'FULL']
    raise SystemExit(run_random_experiment())
