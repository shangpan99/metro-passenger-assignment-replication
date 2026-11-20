from datetime import datetime
from pathlib import Path
import json, os, subprocess, time
import psutil
from common import save_json as save

def fresh(path):
    path=Path(path).resolve()
    if not path.exists(): path.mkdir(parents=True);return path
    if not path.is_dir(): raise NotADirectoryError(path)
    if not any(path.iterdir()): return path
    stamp=datetime.now().strftime('%Y%m%d_%H%M%S')
    for index in range(10000):
        candidate=path.with_name(path.name+'_'+stamp+'_'+str(index))
        try: candidate.mkdir();return candidate
        except FileExistsError: pass
    raise RuntimeError('No fresh output directory available')


def monitored(command,folder,budget,settings):
    folder.mkdir(parents=True)
    started=time.perf_counter()
    peak=0
    forced=False
    # The worker gets a clean subdirectory; parent-owned logs cannot trigger its non-overwrite guard.
    rawout=folder/'run'
    command += ['--output',str(rawout)]
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0) if os.name=='nt' else 0
    with (folder/'console.log').open('w',encoding='utf-8') as log:
        proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,creationflags=flags)
        try: process=psutil.Process(proc.pid)
        except psutil.Error: process=None
        while proc.poll() is None:
            if process is not None:
                try:
                    members=[process]+process.children(recursive=True)
                    total=0
                    for member in members:
                        try: total+=member.memory_info().rss
                        except psutil.Error: pass
                    peak=max(peak,total)
                except psutil.Error: pass
            if time.perf_counter()-started>budget+settings['watchdog_extra_seconds']:
                forced=True
                if process is not None:
                    try:
                        for child in process.children(recursive=True):
                            try: child.kill()
                            except psutil.Error: pass
                    except psutil.Error: pass
                proc.kill()
                break
            time.sleep(settings['memory_sample_interval_seconds'])
        proc.wait()
    summary_path=rawout/'run_summary.json'
    result=json.loads(summary_path.read_text(encoding='utf-8')) if summary_path.exists() else {}
    if forced or proc.returncode or not result:
        result.update(status='WATCHDOG_TIMEOUT_NOT_CERTIFIED' if forced else 'PROCESS_ERROR',
                      local_optimum_certified=False,error='See console.log')
    result.update(exit_code=proc.returncode,process_wall_seconds=time.perf_counter()-started,
                  peak_rss_mb=peak/1024**2 if peak else None,
                  memory_definition='Sampled sum of worker and descendant RSS; excludes batch parent')
    save(folder/'run_summary.json',result)
    return result


