import argparse
import subprocess
import sys
from pathlib import Path

folder = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description='Run one Proposed method paper instance at N=400.')
parser.add_argument('instance', nargs='?', default='N0400_R01_W1', choices=('N0400_R01_W1', 'N0400_R01_W2', 'N0400_R01_W3', 'N0400_R01_W4', 'N0400_R02_W1', 'N0400_R02_W2', 'N0400_R02_W3', 'N0400_R02_W4', 'N0400_R03_W1', 'N0400_R03_W2', 'N0400_R03_W3', 'N0400_R03_W4', 'N0400_R04_W1', 'N0400_R04_W2', 'N0400_R04_W3', 'N0400_R04_W4', 'N0400_R05_W1', 'N0400_R05_W2', 'N0400_R05_W3', 'N0400_R05_W4', 'N0400_R06_W1', 'N0400_R06_W2', 'N0400_R06_W3', 'N0400_R06_W4', 'N0400_R07_W1', 'N0400_R07_W2', 'N0400_R07_W3', 'N0400_R07_W4', 'N0400_R08_W1', 'N0400_R08_W2', 'N0400_R08_W3', 'N0400_R08_W4', 'N0400_R09_W1', 'N0400_R09_W2', 'N0400_R09_W3', 'N0400_R09_W4', 'N0400_R10_W1', 'N0400_R10_W2', 'N0400_R10_W3', 'N0400_R10_W4'))
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--dry-run', action='store_true')
args = parser.parse_args()
command = [sys.executable, str(folder.parents[1] / 'shared/methods/proposed_pb/run_paper.py'),
           '--case', args.instance, '--method', 'proposed', '--output', str(args.output.resolve())]
if args.dry_run:
    command.append('--dry-run')
sys.exit(subprocess.call(command))
