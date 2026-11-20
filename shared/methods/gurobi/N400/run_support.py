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


def stop_tree(proc):
    try:
        parent=psutil.Process(proc.pid)
        for child in parent.children(recursive=True):
            try: child.kill()
            except psutil.Error: pass
        parent.kill()
    except psutil.Error: pass
    proc.wait()


def monitored(command,folder,budget,settings):
    folder.mkdir(parents=True,exist_ok=True)
    if any(folder.iterdir()): raise FileExistsError('Task output must be empty')
    started=time.perf_counter();peak=0;forced=False
    rawout=folder/'run'
    command=list(command)+['--output',str(rawout)]
    flags=getattr(subprocess,'CREATE_NO_WINDOW',0) if os.name=='nt' else 0
    env=dict(os.environ,PYTHONIOENCODING='utf-8',PYTHONDONTWRITEBYTECODE='1',PYTHONUNBUFFERED='1')
    with (folder/'console.log').open('w',encoding='utf-8') as log:
        proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,env=env,creationflags=flags)
        try:
            while proc.poll() is None:
                try:
                    process=psutil.Process(proc.pid)
                    total=0
                    for member in [process]+process.children(recursive=True):
                        try: total+=member.memory_info().rss
                        except psutil.Error: pass
                    peak=max(peak,total)
                except psutil.Error: pass
                try:
                    phase=json.loads((rawout/'lifecycle.json').read_text(encoding='utf-8')).get('stage')
                except (OSError,ValueError): phase=None
                # Solver computation and final export have distinct timing scopes.
                if phase not in ('final_export','done') and time.perf_counter()-started>budget+settings['watchdog_extra_seconds']:
                    forced=True;stop_tree(proc);break
                time.sleep(settings['memory_sample_interval_seconds'])
            proc.wait()
        except BaseException:
            stop_tree(proc)
            raise
    summary_path=rawout/'run_summary.json'
    result=json.loads(summary_path.read_text(encoding='utf-8')) if summary_path.exists() else {}
    if forced or proc.returncode or not result:
        result.update(status='WATCHDOG_TIMEOUT_NOT_CERTIFIED' if forced else 'PROCESS_ERROR',
                      global_optimum_proven=False,error=result.get('error') or 'See console.log')
    result.update(exit_code=proc.returncode,process_wall_seconds=time.perf_counter()-started,
        peak_rss_mb=peak/1024**2 if peak else None,
        memory_definition='Sampled sum of worker and descendant RSS; excludes batch parent')
    save(folder/'run_summary.json',result)
    return result
