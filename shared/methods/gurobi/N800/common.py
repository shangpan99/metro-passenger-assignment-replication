"""Shared serialization, deterministic random streams, and integrity checks."""
import csv
import hashlib
import json
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parent
DATA = ROOT.parents[1] / 'proposed_pb' / 'data' / 'N800_C80'


def departure_window(center, width):
    """Return exactly width integer times; even windows extend earlier first."""
    if not isinstance(width, int) or isinstance(width, bool) or width < 1:
        raise ValueError('Window width must be a positive integer')
    earliest = center - width // 2
    return earliest, earliest + width - 1

def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))

def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name+'.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)+'\n', encoding='utf-8')
    temp.replace(path)

def read_csv(path):
    with Path(path).open(encoding='utf-8-sig', newline='') as f:
        return list(csv.DictReader(f))

def write_csv(path, rows, fields=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', encoding='utf-8-sig', newline='') as f:
        writer = csv.DictWriter(f, fieldnames=fields or list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def stream(master_seed, replicate, purpose):
    label = 'IJOC_RANDOM_V1|{}|{}|{}'.format(master_seed, replicate, purpose)
    seed = int.from_bytes(hashlib.sha256(label.encode('ascii')).digest(), 'big')
    return random.Random(seed), seed

def verify_case(case, data_root=None):
    base = Path(data_root) if data_root else DATA
    inp = base / 'instances' / case
    if not inp.is_dir() or inp.parent.resolve() != (base / 'instances').resolve():
        raise ValueError('Unknown instance: ' + case)
    initial = base / 'initializations' / case
    meta = read_json(initial / 'metadata.json')
    actual = {p.name: sha(p) for p in sorted(inp.glob('input_*.csv'))}
    checks = [actual == meta['input_sha256'],
              sha(inp/'agent_demand.csv') == meta['agent_demand_sha256'],
              sha(initial/'initial_agents.csv') == meta['initial_agents_sha256'],
              sha(initial/'mip_start.json') == meta['mip_start_sha256']]
    if not all(checks):
        raise ValueError('Inputs/initialization have changed: ' + case)
    return meta
