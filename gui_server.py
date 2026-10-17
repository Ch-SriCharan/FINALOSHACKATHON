#!/usr/bin/env python3
"""
Multi-Patient Clinical Telemetry Monitoring Simulator - Web GUI Server
Bridges the Linux POSIX C simulator (SHM, FIFO, Mutexes, Signals, File I/O)
with a modern, high-precision Web UI.

Compatible with native Linux, WSL (Ubuntu-26.04), and Windows host.
"""

import http.server
import socketserver
import json
import os
import sys
import subprocess
import struct
import threading
import time
import re
from urllib.parse import urlparse, parse_qs

PORT = 5050
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
GUI_DIR = os.path.join(SCRIPT_DIR, "gui")
SHM_PATH = "/dev/shm/patient_monitor_shm"
FIFO_PATH = "/tmp/patient_monitor_fifo"
ALERTS_LOG_PATH = os.path.join(SCRIPT_DIR, "alerts.log")
IS_WINDOWS = sys.platform.startswith("win")

def get_wsl_dir():
    if IS_WINDOWS:
        drive, rest = os.path.splitdrive(SCRIPT_DIR)
        if drive:
            return f"/mnt/{drive.rstrip(':').lower()}" + rest.replace('\\', '/')
        return SCRIPT_DIR.replace('\\', '/')
    return SCRIPT_DIR

WSL_DIR = get_wsl_dir()

# Circular buffer for real-time IPC event stream
IPC_EVENTS = []
MAX_IPC_EVENTS = 60
ipc_event_lock = threading.Lock()

def log_ipc_event(source, target, channel, syscall, payload, details, level="info"):
    with ipc_event_lock:
        timestamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"
        evt = {
            "id": len(IPC_EVENTS) + 1,
            "timestamp": timestamp,
            "source": source,
            "target": target,
            "channel": channel,    # "POSIX_SHM", "NAMED_FIFO", "POSIX_SIGNAL", "FILE_IO"
            "syscall": syscall,    # "pthread_mutex_lock()", "write()", "read()", "kill()", "fsync()"
            "payload": payload,
            "details": details,
            "level": level         # "info", "normal", "warning", "critical", "signal"
        }
        IPC_EVENTS.insert(0, evt)
        if len(IPC_EVENTS) > MAX_IPC_EVENTS:
            IPC_EVENTS.pop()
        return evt

# Seed initial educational IPC events
log_ipc_event("Master Process (main.c)", "POSIX SHM (/dev/shm)", "POSIX_SHM", "shm_open() + ftruncate() + mmap()", "496 Bytes Physical Frame", "Provisions process-shared physical memory region mapped across virtual address spaces", "info")
log_ipc_event("Master Process (main.c)", "Named FIFO (/tmp/patient_monitor_fifo)", "NAMED_FIFO", "mkfifo(FIFO_NAME, 0666)", "Channel Provisioned", "Creates named pipe special file for external command injection", "info")

# Simulated state fallback if C simulator is not running
SIM_STATE = {
    "active": False,
    "uptime_sec": 0,
    "master_pid": 0,
    "total_sensor_updates": 0,
    "total_alerts_triggered": 0,
    "sensor_pids": [0, 0, 0],
    "alert_detector_pid": 0,
    "fifo_listener_pid": 0,
    "patients": [
        {
            "patient_id": 101,
            "name": "Alice Walker (Cardio)",
            "heart_rate": 72.0,
            "spo2": 98.6,
            "status": 0,
            "status_str": "NORMAL",
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "sequence_number": 1,
            "sensor_pid": 1001,
            "locked": False
        },
        {
            "patient_id": 102,
            "name": "Bob Martinez (ICU-1)",
            "heart_rate": 82.5,
            "spo2": 96.2,
            "status": 0,
            "status_str": "NORMAL",
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "sequence_number": 1,
            "sensor_pid": 1002,
            "locked": False
        },
        {
            "patient_id": 103,
            "name": "Clara Zhao (Post-Op)",
            "heart_rate": 66.0,
            "spo2": 97.4,
            "status": 0,
            "status_str": "NORMAL",
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "sequence_number": 1,
            "sensor_pid": 1003,
            "locked": False
        }
    ]
}

def run_wsl_cmd(cmd_str):
    """Run command inside WSL distribution or native Linux"""
    try:
        if not IS_WINDOWS:
            res = subprocess.run(["bash", "-c", cmd_str], capture_output=True, text=True, timeout=10)
            return res.returncode, res.stdout, res.stderr
        full_cmd = ["wsl", "-e", "bash", "-c", cmd_str]
        res = subprocess.run(full_cmd, capture_output=True, text=True, timeout=10)
        return res.returncode, res.stdout, res.stderr
    except Exception as e:
        return -1, "", str(e)

def is_c_simulator_running():
    """Check if patient_monitor process is actively running in WSL or native Linux"""
    rc, out, _ = run_wsl_cmd("pgrep -f 'patient_monitor' | head -n 1")
    return rc == 0 and out.strip() != ""

def read_real_shm():
    """Read and decode the 496-byte POSIX shared memory from WSL or native Linux"""
    python_script = """
import struct, json, os

if not os.path.exists('/dev/shm/patient_monitor_shm'):
    print(json.dumps({'error': 'SHM_NOT_FOUND'}))
    exit(0)

try:
    with open('/dev/shm/patient_monitor_shm', 'rb') as f:
        buf = f.read(496)

    if len(buf) < 496:
        print(json.dumps({'error': 'INCOMPLETE_SHM'}))
        exit(0)

    sim_active = struct.unpack_from('<i', buf, 0)[0]
    total_updates = struct.unpack_from('<Q', buf, 456)[0]
    total_alerts = struct.unpack_from('<Q', buf, 464)[0]
    master_pid = struct.unpack_from('<i', buf, 472)[0]
    s_pids = [
        struct.unpack_from('<i', buf, 476)[0],
        struct.unpack_from('<i', buf, 480)[0],
        struct.unpack_from('<i', buf, 484)[0]
    ]
    alert_pid = struct.unpack_from('<i', buf, 488)[0]
    fifo_pid = struct.unpack_from('<i', buf, 492)[0]

    patients = []
    status_map = {0: 'NORMAL', 1: 'WARNING', 2: 'CRITICAL'}

    for i in range(3):
        base = 48 + i * 136
        pid = struct.unpack_from('<i', buf, base)[0]
        name = buf[base+4:base+36].split(b'\\x00')[0].decode('utf-8', 'ignore')
        hr = struct.unpack_from('<f', buf, base+36)[0]
        spo2 = struct.unpack_from('<f', buf, base+40)[0]
        st = struct.unpack_from('<i', buf, base+44)[0]
        ts = buf[base+48:base+80].split(b'\\x00')[0].decode('utf-8', 'ignore')
        seq = struct.unpack_from('<Q', buf, base+80)[0]
        sensor_pid = struct.unpack_from('<i', buf, base+88)[0]
        
        patients.append({
            'patient_id': pid,
            'name': name,
            'heart_rate': round(hr, 1),
            'spo2': round(spo2, 1),
            'status': st,
            'status_str': status_map.get(st, 'NORMAL'),
            'timestamp': ts,
            'sequence_number': seq,
            'sensor_pid': sensor_pid,
            'locked': False
        })

    print(json.dumps({
        'active': bool(sim_active),
        'master_pid': master_pid,
        'total_sensor_updates': total_updates,
        'total_alerts_triggered': total_alerts,
        'sensor_pids': s_pids,
        'alert_detector_pid': alert_pid,
        'fifo_listener_pid': fifo_pid,
        'patients': patients,
        'source': 'POSIX_SHARED_MEMORY'
    }))
except Exception as ex:
    print(json.dumps({'error': str(ex)}))
"""
    try:
        if IS_WINDOWS:
            res = subprocess.run(["wsl", "-e", "python3", "-c", python_script], capture_output=True, text=True, timeout=5)
        else:
            res = subprocess.run(["python3", "-c", python_script], capture_output=True, text=True, timeout=5)
        if res.returncode == 0 and res.stdout.strip():
            data = json.loads(res.stdout.strip())
            if "error" not in data:
                return data
    except Exception:
        pass
    return None

def send_fifo_command(cmd_line):
    """Write command to /tmp/patient_monitor_fifo in WSL"""
    escaped = cmd_line.replace('"', '\\"')
    bash_cmd = f'echo "{escaped}" > /tmp/patient_monitor_fifo'
    rc, out, err = run_wsl_cmd(bash_cmd)
    return rc == 0

def execute_race_demo(mode):
    """Run race_demo (unsync or sync) and parse numerical metrics"""
    valid_mode = "sync" if mode == "sync" else "unsync"
    cmd = f"cd '{WSL_DIR}' && ./race_demo {valid_mode}"
    rc, out, err = run_wsl_cmd(cmd)
    
    result = {
        "mode": valid_mode,
        "success": rc == 0,
        "execution_time_ms": 0.0,
        "validation_reads": 0,
        "torn_reads": 0,
        "expected_counter": 80000,
        "actual_counter": 0,
        "lost_updates": 0,
        "raw_output": out
    }
    
    # Parse metrics from stdout
    m_time = re.search(r"Execution Time:\s+([\d\.]+)\s+ms", out)
    if m_time:
        result["execution_time_ms"] = float(m_time.group(1))
        
    m_reads = re.search(r"Total Validation Reads:\s+(\d+)", out)
    if m_reads:
        result["validation_reads"] = int(m_reads.group(1))
        
    m_torn = re.search(r"Torn Reads.*?:\s+(\d+)", out)
    if m_torn:
        result["torn_reads"] = int(m_torn.group(1))
        
    m_expected = re.search(r"Expected Update Counter:\s+(\d+)", out)
    if m_expected:
        result["expected_counter"] = int(m_expected.group(1))
        
    m_actual = re.search(r"Actual Update Counter:\s+(\d+)", out)
    if m_actual:
        result["actual_counter"] = int(m_actual.group(1))
        
    m_lost = re.search(r"Lost Updates.*?:\s+(\d+)", out)
    if m_lost:
        result["lost_updates"] = int(m_lost.group(1))
        
    return result

def read_alerts_log():
    """Read alerts.log from local workspace or WSL"""
    alerts = []
    log_file = ALERTS_LOG_PATH
    if os.path.exists(log_file):
        with open(log_file, "r", encoding="utf-8", errors="ignore") as f:
            lines = f.readlines()
            for line in lines:
                line = line.strip()
                if not line:
                    continue
                m = re.match(r"\[([\d\- :]+)\]\s+\[([A-Z]+)\]\s+Patient ID:\s+(\d+)\s*\((.*?)\)\s*\|\s*HR:\s*([\d\.]+)\s*BPM\s*\|\s*SpO2:\s*([\d\.]+)%\s*\|\s*Seq:\s*(\d+)\s*\|\s*Sensor PID:\s*(\d+)\s*\|\s*(.*)", line)
                if m:
                    alerts.append({
                        "timestamp": m.group(1),
                        "level": m.group(2),
                        "patient_id": int(m.group(3)),
                        "patient_name": m.group(4),
                        "heart_rate": float(m.group(5)),
                        "spo2": float(m.group(6)),
                        "sequence": int(m.group(7)),
                        "sensor_pid": int(m.group(8)),
                        "diagnosis": m.group(9)
                    })
                else:
                    alerts.append({
                        "timestamp": "",
                        "level": "INFO",
                        "raw": line
                    })
    return alerts

# Background telemetry generator for fallback when C simulator is stopped
tick_counter = 0
def simulation_tick():
    global tick_counter
    import random
    while True:
        time.sleep(1.0)
        tick_counter += 1
        c_running = is_c_simulator_running()
        if not c_running:
            SIM_STATE["uptime_sec"] += 1
            SIM_STATE["total_sensor_updates"] += 3
            for p in SIM_STATE["patients"]:
                delta_hr = random.uniform(-1.0, 1.0)
                delta_spo2 = random.uniform(-0.3, 0.3)
                p["heart_rate"] = max(40.0, min(160.0, round(p["heart_rate"] + delta_hr, 1)))
                p["spo2"] = max(75.0, min(100.0, round(p["spo2"] + delta_spo2, 1)))
                p["sequence_number"] += 1
                p["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S")
                
                hr = p["heart_rate"]
                spo2 = p["spo2"]
                prev_status = p["status"]
                if hr < 50.0 or hr > 120.0 or spo2 < 90.0:
                    p["status"] = 2
                    p["status_str"] = "CRITICAL"
                elif hr < 60.0 or hr > 100.0 or spo2 < 95.0:
                    p["status"] = 1
                    p["status_str"] = "WARNING"
                else:
                    p["status"] = 0
                    p["status_str"] = "NORMAL"

                # Log periodic telemetry transfer
                if tick_counter % 3 == 0:
                    log_ipc_event(
                        source=f"Sensor PID {p['sensor_pid']} (Bed {p['patient_id']})",
                        target=f"POSIX SHM Bed {p['patient_id']} (Offset 0x{48 + (p['patient_id']-101)*136:03X})",
                        channel="POSIX_SHM",
                        syscall="pthread_mutex_lock(); memcpy(); pthread_mutex_unlock();",
                        payload=f"HR={p['heart_rate']:.1f} BPM, SpO2={p['spo2']:.1f}%, Seq={p['sequence_number']}",
                        details=f"Committed telemetry frame to shared memory partition under mutex lock",
                        level="normal"
                    )

                # Log alert detection if condition abnormal
                if p["status"] != 0 and prev_status == 0:
                    SIM_STATE["total_alerts_triggered"] += 1
                    log_ipc_event(
                        source="Alert Detector Process (PID 1004)",
                        target="Persistent Disk Storage (alerts.log)",
                        channel="FILE_IO",
                        syscall="open(O_APPEND); write(); fsync(fd); close();",
                        payload=f"ALERT: Bed {p['patient_id']} ({p['status_str']}) - HR {p['heart_rate']:.1f}, SpO2 {p['spo2']:.1f}%",
                        details="Threshold violation detected during polling; flushed immediately to persistent storage",
                        level="critical" if p["status"] == 2 else "warning"
                    )

sim_thread = threading.Thread(target=simulation_tick, daemon=True)
sim_thread.start()

class PatientMonitorHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=GUI_DIR, **kwargs)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/status":
            self.handle_api_status()
        elif path == "/api/alerts":
            self.handle_api_alerts()
        elif path == "/api/shm_layout":
            self.handle_api_shm_layout()
        elif path == "/api/ipc_events":
            self.handle_api_ipc_events()
        elif path == "/" or path == "/index.html":
            self.path = "/index.html"
            return super().do_GET()
        else:
            return super().do_GET()

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode("utf-8") if length > 0 else ""
        
        try:
            req_data = json.loads(body) if body else {}
        except Exception:
            req_data = {}

        if path == "/api/start":
            self.handle_api_start(req_data)
        elif path == "/api/stop":
            self.handle_api_stop()
        elif path == "/api/inject":
            self.handle_api_inject(req_data)
        elif path == "/api/signal":
            self.handle_api_signal(req_data)
        elif path == "/api/race":
            self.handle_api_race(req_data)
        elif path == "/api/trace_transfer":
            self.handle_api_trace_transfer(req_data)
        else:
            self.send_error(404, "Endpoint not found")

    def handle_api_status(self):
        shm_data = read_real_shm()
        c_running = is_c_simulator_running()

        if c_running and shm_data:
            response = {
                "simulator_online": True,
                "engine": "POSIX_C_SHM",
                "data": shm_data
            }
        else:
            response = {
                "simulator_online": False,
                "engine": "SIMULATION_FALLBACK",
                "message": "C Multi-Process Simulator is offline. Click 'Start Simulator' to launch.",
                "data": SIM_STATE
            }

        self.send_json_response(response)

    def handle_api_start(self, data):
        duration = data.get("duration", 0)
        if is_c_simulator_running():
            self.send_json_response({"success": True, "message": "Simulator is already running."})
            return

        cmd = f"cd '{WSL_DIR}' && nohup ./patient_monitor"
        if duration > 0:
            cmd += f" --demo-duration {duration}"
        cmd += " >/dev/null 2>&1 </dev/null & disown -a"

        rc, _, err = run_wsl_cmd(cmd)
        time.sleep(1.0)
        c_running = is_c_simulator_running()

        log_ipc_event(
            source="Web Controller",
            target="Master Process (main.c)",
            channel="POSIX_SIGNAL",
            syscall="fork() + execve('./patient_monitor')",
            payload="Spawn Master & 5 Child Daemons",
            details="Spawned master process, 3 patient sensors, alert detector, and FIFO listener",
            level="info"
        )

        self.send_json_response({
            "success": c_running,
            "message": "C Simulator started successfully." if c_running else f"Failed to start: {err}"
        })

    def handle_api_stop(self):
        if is_c_simulator_running():
            send_fifo_command("shutdown")
            time.sleep(0.5)
            run_wsl_cmd("pkill -SIGINT -f 'patient_monitor'")
            time.sleep(0.5)
            log_ipc_event(
                source="Web Controller",
                target="Master Process (main.c)",
                channel="POSIX_SIGNAL",
                syscall="kill(master_pid, SIGINT)",
                payload="SIGINT (2)",
                details="Dispatched graceful shutdown request to master process; reaped all child processes",
                level="warning"
            )
            self.send_json_response({"success": True, "message": "Simulation shutdown signal dispatched."})
        else:
            self.send_json_response({"success": True, "message": "Simulator is already stopped."})

    def handle_api_inject(self, data):
        patient_id = int(data.get("patient_id", 101))
        hr = float(data.get("heart_rate", 72.0))
        spo2 = float(data.get("spo2", 98.0))

        # Always update local SIM_STATE
        for p in SIM_STATE["patients"]:
            if p["patient_id"] == patient_id:
                p["heart_rate"] = hr
                p["spo2"] = spo2
                p["sequence_number"] += 1
                p["timestamp"] = time.strftime("%Y-%m-%d %H:%M:%S")
                if hr < 50.0 or hr > 120.0 or spo2 < 90.0:
                    p["status"] = 2
                    p["status_str"] = "CRITICAL"
                elif hr < 60.0 or hr > 100.0 or spo2 < 95.0:
                    p["status"] = 1
                    p["status_str"] = "WARNING"
                else:
                    p["status"] = 0
                    p["status_str"] = "NORMAL"

        c_running = is_c_simulator_running()
        fifo_sent = False
        if c_running:
            cmd = f"inject {patient_id} {hr} {spo2}"
            fifo_sent = send_fifo_command(cmd)

        # Log IPC Step 1: External command into Named FIFO
        log_ipc_event(
            source="External Admin / Web GUI",
            target="Named FIFO (/tmp/patient_monitor_fifo)",
            channel="NAMED_FIFO",
            syscall=f"write(fifo_fd, 'inject {patient_id} {hr:.1f} {spo2:.1f}\\n', len)",
            payload=f"inject {patient_id} {hr:.1f} {spo2:.1f}",
            details="Command bytes written to kernel pipe buffer",
            level="info"
        )

        # Log IPC Step 2: FIFO Process wakes up and modifies Shared Memory under Mutex
        offset = 48 + (patient_id - 101) * 136
        log_ipc_event(
            source="FIFO Listener Process",
            target=f"POSIX SHM Bed {patient_id} (0x{offset:03X})",
            channel="POSIX_SHM",
            syscall=f"safe_mutex_lock(&patients[{patient_id-101}].lock) -> atomic write -> unlock",
            payload=f"HR={hr:.1f} BPM, SpO2={spo2:.1f}%",
            details="Acquired process-shared robust mutex, committed abnormal telemetry snapshot, released lock",
            level="warning" if (hr < 50 or hr > 120 or spo2 < 90) else "normal"
        )

        # Log IPC Step 3: Alert Detector catches abnormal reading
        if hr < 50 or hr > 120 or spo2 < 90:
            log_ipc_event(
                source="Alert Detector Process",
                target="Persistent File (alerts.log)",
                channel="FILE_IO",
                syscall="write(fd, alert_buf, len) + fsync(fd)",
                payload=f"CRITICAL ALERT: Patient {patient_id} (HR: {hr:.1f}, SpO2: {spo2:.1f}%)",
                details="Detected acute anomaly during high-speed SHM polling; flushed record to persistent disk log",
                level="critical"
            )

        self.send_json_response({
            "success": True,
            "patient_id": patient_id,
            "heart_rate": hr,
            "spo2": spo2,
            "c_running": c_running,
            "fifo_dispatched": fifo_sent,
            "message": f"Injected vitals into Patient {patient_id} (HR: {hr} BPM, SpO2: {spo2}%)"
        })

    def handle_api_signal(self, data):
        patient_id = int(data.get("patient_id", 101))
        c_running = is_c_simulator_running()
        if c_running:
            send_fifo_command(f"signal {patient_id}")
            log_ipc_event(
                source="FIFO Listener Process",
                target=f"Sensor Process (Bed {patient_id})",
                channel="POSIX_SIGNAL",
                syscall="kill(sensor_pid, SIGUSR1)",
                payload="SIGUSR1 (Signal 10)",
                details="Asynchronous POSIX signal sent to sensor process to trigger status report / calibration",
                level="signal"
            )
            self.send_json_response({"success": True, "message": f"SIGUSR1 dispatched to Patient {patient_id} sensor process"})
        else:
            log_ipc_event(
                source="Web Controller",
                target=f"Sensor PID {1000 + patient_id - 100} (Bed {patient_id})",
                channel="POSIX_SIGNAL",
                syscall="kill(sensor_pid, SIGUSR1)",
                payload="SIGUSR1 (Signal 10)",
                details="Simulated asynchronous signal delivery to target sensor process",
                level="signal"
            )
            self.send_json_response({"success": True, "message": f"Signal simulated for Patient {patient_id}"})

    def handle_api_race(self, data):
        mode = data.get("mode", "unsync")
        res = execute_race_demo(mode)

        log_ipc_event(
            source="Concurrency Lab",
            target="Shared Memory (/race_demo_shm)",
            channel="POSIX_SHM",
            syscall="pthread_mutex_lock()" if mode == "sync" else "UNSYNCHRONIZED (NO LOCKS)",
            payload=f"Mode: {mode.upper()} | Updates: {res['actual_counter']}/80000",
            details=f"Outcome: {res['lost_updates']} lost updates, {res['torn_reads']} torn reads",
            level="normal" if mode == "sync" else "critical"
        )

        self.send_json_response(res)

    def handle_api_trace_transfer(self, data):
        """Generates a complete step-by-step trace of information transfer across all components"""
        patient_id = int(data.get("patient_id", 101))
        hr = float(data.get("heart_rate", 38.0))
        spo2 = float(data.get("spo2", 84.0))

        steps = [
            {
                "step": 1,
                "stage": "EXTERNAL_COMMAND",
                "source": "Terminal / Web UI",
                "target": "Named FIFO (/tmp/patient_monitor_fifo)",
                "channel": "Named Pipe (mkfifo)",
                "syscall": f"open('/tmp/patient_monitor_fifo', O_WRONLY) -> write('inject {patient_id} {hr} {spo2}\\n')",
                "payload": f"inject {patient_id} {hr} {spo2}",
                "explanation": "External command enters the OS filesystem via the Named Pipe inode. The kernel writes payload into a 64KB circular pipe buffer in RAM."
            },
            {
                "step": 2,
                "stage": "PIPE_READ_WAKEUP",
                "source": "Named FIFO (/tmp/patient_monitor_fifo)",
                "target": "FIFO Listener Process (PID)",
                "channel": "Named Pipe IPC",
                "syscall": "read(fifo_fd, line_buf, 256) -> sscanf(line_buf, 'inject %d %f %f')",
                "payload": f"Parsed: ID={patient_id}, HR={hr}, SpO2={spo2}",
                "explanation": "The blocked FIFO listener process unblocks from read() as kernel pipe buffer delivers the command string. Command is parsed into numeric vitals."
            },
            {
                "step": 3,
                "stage": "MUTEX_ACQUISITION",
                "source": "FIFO Listener Process",
                "target": f"Process-Shared Mutex (patients[{patient_id-101}].lock)",
                "channel": "POSIX Mutex (PTHREAD_PROCESS_SHARED)",
                "syscall": "pthread_mutex_lock(&shm->patients[index].lock)",
                "payload": "Lock Acquired (Atomic Test-and-Set / Futex)",
                "explanation": "To prevent torn reads or race conditions, the writer acquires the per-patient robust mutex. Any concurrent readers will safely wait."
            },
            {
                "step": 4,
                "stage": "SHARED_MEMORY_UPDATE",
                "source": "FIFO Listener Process",
                "target": f"POSIX Shared Memory Frame (Offset 0x{48 + (patient_id-101)*136:03X})",
                "channel": "Direct Memory Transfer (mmap)",
                "syscall": "shm->patients[index].heart_rate = hr; shm->patients[index].spo2 = spo2; seq++;",
                "payload": f"Written: HR={hr} BPM, SpO2={spo2}%, Seq Increment",
                "explanation": "Direct virtual-to-physical memory write without kernel context-switch copy overhead. Physical RAM page frame is instantly updated."
            },
            {
                "step": 5,
                "stage": "MUTEX_RELEASE",
                "source": "FIFO Listener Process",
                "target": f"Process-Shared Mutex (patients[{patient_id-101}].lock)",
                "channel": "POSIX Mutex",
                "syscall": "pthread_mutex_unlock(&shm->patients[index].lock)",
                "payload": "Lock Released",
                "explanation": "Critical section completed. Mutex is unlocked and waiting processes (e.g. Alert Detector, Monitor) are unblocked."
            },
            {
                "step": 6,
                "stage": "DETECTOR_POLL_SNAPSHOT",
                "source": f"POSIX Shared Memory (Bed {patient_id})",
                "target": "Alert Detector Process",
                "channel": "POSIX SHM Snapshot",
                "syscall": "safe_mutex_lock() -> evaluate_vitals_status() -> safe_mutex_unlock()",
                "payload": f"Evaluated: CRITICAL: Severe Bradycardia (HR={hr}) & Hypoxemia (SpO2={spo2}%)",
                "explanation": "Alert detector polls the shared memory, takes an atomic snapshot, and detects that vitals breached the physiological safety limits."
            },
            {
                "step": 7,
                "stage": "PERSISTENT_FILE_LOGGING",
                "source": "Alert Detector Process",
                "target": "Hard Disk / File System (alerts.log)",
                "channel": "Low-level POSIX File I/O",
                "syscall": "open('alerts.log', O_WRONLY|O_CREAT|O_APPEND) -> write(fd, buf, len) -> fsync(fd)",
                "payload": f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] [CRITICAL] Patient {patient_id} | HR: {hr} | SpO2: {spo2}%",
                "explanation": "Immediate kernel persistence via write() and fsync() to ensure zero telemetry loss even in power failure. Duplicate suppression prevents log flooding."
            },
            {
                "step": 8,
                "stage": "DASHBOARD_RENDERING",
                "source": "POSIX Shared Memory",
                "target": "Live Clinical Telemetry UI",
                "channel": "Web UI Oscilloscope & Alarms",
                "syscall": "Canvas Redraw (ECG Bradycardia Waveform) + Red Alert Flash + Audio Tone",
                "payload": "UI Dashboard Updated in Real-Time",
                "explanation": "The user interface displays code-red emergency status, stretches the ECG waveform to 38 BPM spacing, and triggers the clinical alert audio tone."
            }
        ]

        self.send_json_response({
            "success": True,
            "patient_id": patient_id,
            "heart_rate": hr,
            "spo2": spo2,
            "steps": steps
        })

    def handle_api_alerts(self):
        alerts = read_alerts_log()
        self.send_json_response({"alerts": alerts, "total": len(alerts)})

    def handle_api_ipc_events(self):
        with ipc_event_lock:
            events_copy = list(IPC_EVENTS)
        self.send_json_response({"events": events_copy, "total": len(events_copy)})

    def handle_api_shm_layout(self):
        layout = {
            "shm_name": "/dev/shm/patient_monitor_shm",
            "shm_total_bytes": 496,
            "structure_name": "SharedMemory",
            "fields": [
                {"name": "simulation_active", "offset": 0, "size": 4, "type": "int32_t (sig_atomic_t)", "desc": "1 = active simulation, 0 = initiate graceful shutdown"},
                {"name": "padding / align", "offset": 4, "size": 4, "type": "uint8_t[4]", "desc": "64-bit boundary alignment padding"},
                {"name": "global_lock", "offset": 8, "size": 40, "type": "pthread_mutex_t", "desc": "Process-shared robust mutex for global aggregates"},
                {"name": "patients[0] (Bed 101)", "offset": 48, "size": 136, "type": "PatientRecord", "desc": "Telemetry record for Patient 101 (Alice Walker)"},
                {"name": "patients[1] (Bed 102)", "offset": 184, "size": 136, "type": "PatientRecord", "desc": "Telemetry record for Patient 102 (Bob Martinez)"},
                {"name": "patients[2] (Bed 103)", "offset": 320, "size": 136, "type": "PatientRecord", "desc": "Telemetry record for Patient 103 (Clara Zhao)"},
                {"name": "total_sensor_updates", "offset": 456, "size": 8, "type": "uint64_t", "desc": "Aggregate counter of updates generated by all sensors"},
                {"name": "total_alerts_triggered", "offset": 464, "size": 8, "type": "uint64_t", "desc": "Aggregate count of abnormal condition alerts persisted"},
                {"name": "master_pid", "offset": 472, "size": 4, "type": "pid_t", "desc": "PID of orchestrator master process (main.c)"},
                {"name": "sensor_pids[0]", "offset": 476, "size": 4, "type": "pid_t", "desc": "PID of independent sensor process for Bed 101"},
                {"name": "sensor_pids[1]", "offset": 480, "size": 4, "type": "pid_t", "desc": "PID of independent sensor process for Bed 102"},
                {"name": "sensor_pids[2]", "offset": 484, "size": 4, "type": "pid_t", "desc": "PID of independent sensor process for Bed 103"},
                {"name": "alert_detector_pid", "offset": 488, "size": 4, "type": "pid_t", "desc": "PID of alert detection & file logging process"},
                {"name": "fifo_listener_pid", "offset": 492, "size": 4, "type": "pid_t", "desc": "PID of Named FIFO external control listener"}
            ],
            "patient_record_layout": [
                {"name": "patient_id", "offset": 0, "size": 4, "type": "int", "desc": "Clinical identifier (e.g., 101)"},
                {"name": "name", "offset": 4, "size": 32, "type": "char[32]", "desc": "Patient label / room description"},
                {"name": "heart_rate", "offset": 36, "size": 4, "type": "float", "desc": "Cardiac pulse rate in BPM"},
                {"name": "spo2", "offset": 40, "size": 4, "type": "float", "desc": "Blood oxygen saturation percentage"},
                {"name": "status", "offset": 44, "size": 4, "type": "int (enum)", "desc": "0=NORMAL, 1=WARNING, 2=CRITICAL"},
                {"name": "timestamp", "offset": 48, "size": 32, "type": "char[32]", "desc": "ISO formatted update timestamp"},
                {"name": "sequence_number", "offset": 80, "size": 8, "type": "uint64_t", "desc": "Monotonically increasing atomic sequence counter"},
                {"name": "sensor_pid", "offset": 88, "size": 4, "type": "pid_t", "desc": "PID of writer process"},
                {"name": "lock", "offset": 96, "size": 40, "type": "pthread_mutex_t", "desc": "Process-shared robust mutex protecting this bed"}
            ]
        }
        self.send_json_response(layout)

    def send_json_response(self, data):
        body = json.dumps(data).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

def run_server():
    os.makedirs(GUI_DIR, exist_ok=True)
    server_address = ("0.0.0.0", PORT)
    with socketserver.TCPServer(server_address, PatientMonitorHandler) as httpd:
        print(f"[GUI Server] Serving Multi-Patient Monitor Web Interface at http://localhost:{PORT}")
        httpd.serve_forever()

if __name__ == "__main__":
    run_server()
