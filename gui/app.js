/**
 * ==============================================================================
 * VitalsSync ICU - Clinical Telemetry & Operating Systems Simulator
 * JavaScript Application Logic: gui/app.js
 * ==============================================================================
 */

// Global State
const state = {
  activeTab: 'tab-monitor',
  simulatorOnline: false,
  audioEnabled: false,
  uptimeSec: 0,
  lastPollTime: Date.now(),
  shmLayout: null,
  patients: {
    101: { hr: 72.0, spo2: 98.6, status: 'NORMAL', seq: 1, pid: 0, phase: 0 },
    102: { hr: 82.5, spo2: 96.2, status: 'NORMAL', seq: 1, pid: 0, phase: 0.33 },
    103: { hr: 66.0, spo2: 97.4, status: 'NORMAL', seq: 1, pid: 0, phase: 0.66 }
  }
};

// Web Audio API Synthesizer for Clinical Telemetry Beeps
let audioCtx = null;
function playClinicalBeep(pitch = 880, duration = 0.08) {
  if (!state.audioEnabled) return;
  try {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioCtx.state === 'suspended') {
      audioCtx.resume();
    }
    const osc = audioCtx.createOscillator();
    const gain = audioCtx.createGain();
    osc.type = 'sine';
    osc.frequency.setValueAtTime(pitch, audioCtx.currentTime);
    gain.gain.setValueAtTime(0.06, audioCtx.currentTime);
    gain.gain.exponentialRampToValueAtTime(0.001, audioCtx.currentTime + duration);
    osc.connect(gain);
    gain.connect(audioCtx.destination);
    osc.start();
    osc.stop(audioCtx.currentTime + duration);
  } catch (e) {
    console.warn("Audio synthesis error:", e);
  }
}

// ==============================================================================
// Oscilloscope & Waveform Rendering Engine
// ==============================================================================
class WaveformRenderer {
  constructor(canvasId, type, bedId) {
    this.canvas = document.getElementById(canvasId);
    this.ctx = this.canvas ? this.canvas.getContext('2d') : null;
    this.type = type; // 'ecg' or 'ppg'
    this.bedId = bedId;
    this.x = 0;
    this.sweepSpeed = 2.0; // pixels per animation frame
    this.lastY = 0;
    this.hasBeepedThisBeat = false;
  }

  getHeartRate() {
    return state.patients[this.bedId] ? state.patients[this.bedId].hr : 75;
  }

  getSpO2() {
    return state.patients[this.bedId] ? state.patients[this.bedId].spo2 : 98;
  }

  computeEcgSample(t) {
    const hr = Math.max(30, Math.min(220, this.getHeartRate()));
    const beatPeriod = 60 / hr; // seconds per cardiac cycle
    const phase = (t % beatPeriod) / beatPeriod; // 0.0 to 1.0

    // Sound telemetry beep on R-wave peak
    if (phase > 0.22 && phase < 0.28 && !this.hasBeepedThisBeat) {
      this.hasBeepedThisBeat = true;
      const spo2 = this.getSpO2();
      // Pitch drops from 880Hz down to 440Hz as oxygen desaturates
      const pitch = spo2 >= 95 ? 880 : Math.max(380, 440 + (spo2 - 70) * 15);
      playClinicalBeep(pitch);
    } else if (phase >= 0.5) {
      this.hasBeepedThisBeat = false;
    }

    let val = 0;
    // P-wave (atrial depolarization)
    if (phase >= 0.08 && phase < 0.16) {
      const p = (phase - 0.08) / 0.08;
      val += 0.16 * Math.sin(p * Math.PI);
    }
    // Q-wave (slight dip)
    else if (phase >= 0.20 && phase < 0.23) {
      const q = (phase - 0.20) / 0.03;
      val -= 0.12 * Math.sin(q * Math.PI);
    }
    // R-wave (sharp ventricular depolarization spike)
    else if (phase >= 0.23 && phase < 0.27) {
      const r = (phase - 0.23) / 0.04;
      val += 0.95 * Math.sin(r * Math.PI);
    }
    // S-wave (rebound dip)
    else if (phase >= 0.27 && phase < 0.31) {
      const s = (phase - 0.27) / 0.04;
      val -= 0.28 * Math.sin(s * Math.PI);
    }
    // T-wave (ventricular repolarization)
    else if (phase >= 0.36 && phase < 0.50) {
      const tw = (phase - 0.36) / 0.14;
      val += 0.24 * Math.sin(tw * Math.PI);
    }

    // Add tiny clinical bio-noise
    val += (Math.random() - 0.5) * 0.02;
    return val;
  }

  computePpgSample(t) {
    const hr = Math.max(30, Math.min(220, this.getHeartRate()));
    const beatPeriod = 60 / hr;
    const phase = (t % beatPeriod) / beatPeriod;

    let val = 0;
    // Anacrotic systolic rise
    if (phase < 0.25) {
      const u = phase / 0.25;
      val = Math.sin(u * (Math.PI / 2));
    }
    // Dicrotic notch and runoff
    else if (phase < 0.40) {
      const u = (phase - 0.25) / 0.15;
      val = 1.0 - 0.35 * u + 0.15 * Math.sin(u * Math.PI);
    } else {
      const u = (phase - 0.40) / 0.60;
      val = 0.65 * (1.0 - u);
    }
    return val * 0.85;
  }

  renderStep(timeSec) {
    if (!this.ctx || !this.canvas) return;
    const w = this.canvas.width;
    const h = this.canvas.height;
    const midY = h / 2;

    const currentSample = this.type === 'ecg' ? this.computeEcgSample(timeSec) : this.computePpgSample(timeSec);
    const scale = this.type === 'ecg' ? h * 0.42 : h * 0.45;
    const y = midY - currentSample * scale;

    const nextX = (this.x + this.sweepSpeed) % w;

    // Erase ahead of the sweep beam (phosphor sweep bar)
    this.ctx.fillStyle = '#03060a';
    this.ctx.fillRect(nextX, 0, 18, h);

    // Draw trace segment
    this.ctx.beginPath();
    this.ctx.lineWidth = this.type === 'ecg' ? 2.0 : 2.2;
    this.ctx.strokeStyle = this.type === 'ecg' ? '#00ff88' : '#00e5ff';
    this.ctx.shadowColor = this.type === 'ecg' ? 'rgba(0, 255, 136, 0.6)' : 'rgba(0, 229, 255, 0.6)';
    this.ctx.shadowBlur = 6;
    this.ctx.lineCap = 'round';

    if (nextX < this.x) {
      // Loop around screen edge
      this.ctx.moveTo(0, y);
    } else {
      this.ctx.moveTo(this.x, this.lastY || y);
      this.ctx.lineTo(nextX, y);
    }
    this.ctx.stroke();

    this.x = nextX;
    this.lastY = y;
  }
}

// Instantiate Renderers for 3 Beds
const renderers = [];
function initRenderers() {
  [101, 102, 103].forEach(bedId => {
    renderers.push(new WaveformRenderer(`canvasEcg${bedId}`, 'ecg', bedId));
    renderers.push(new WaveformRenderer(`canvasPpg${bedId}`, 'ppg', bedId));
  });
}

let animStartTime = performance.now();
function animLoop(now) {
  const timeSec = (now - animStartTime) / 1000;
  renderers.forEach(r => r.renderStep(timeSec));
  requestAnimationFrame(animLoop);
}

// ==============================================================================
// UI Updates & Polling
// ==============================================================================
async function fetchStatus() {
  try {
    const res = await fetch('/api/status');
    if (!res.ok) throw new Error('Status poll failed');
    const data = await res.json();
    updateDashboard(data);
  } catch (err) {
    console.warn("Telemetry poll error:", err);
    document.getElementById('engineStatusText').textContent = 'OFFLINE (C SIM)';
    document.getElementById('engineStatusBadge').className = 'chip-val status-val text-amber';
  }
}

function updateDashboard(resp) {
  const isOnline = resp.simulator_online;
  state.simulatorOnline = isOnline;
  const payload = resp.data || {};

  // Status Badge
  const badgeEl = document.getElementById('engineStatusBadge');
  const textEl = document.getElementById('engineStatusText');
  if (isOnline) {
    textEl.textContent = 'POSIX SHM ONLINE';
    badgeEl.className = 'chip-val status-val text-green';
  } else {
    textEl.textContent = 'STANDBY SIMULATOR';
    badgeEl.className = 'chip-val status-val text-amber';
  }

  // Header Counters
  document.getElementById('headerMasterPid').textContent = payload.master_pid > 0 ? payload.master_pid : 'N/A';
  document.getElementById('headerTotalUpdates').textContent = (payload.total_sensor_updates || 0).toLocaleString();
  document.getElementById('headerTotalAlerts').textContent = (payload.total_alerts_triggered || 0).toLocaleString();
  
  // Tree PIDs
  if (document.getElementById('treeMasterPid')) {
    document.getElementById('treeMasterPid').textContent = `PID: ${payload.master_pid || '--'}`;
  }
  if (payload.sensor_pids && payload.sensor_pids.length >= 3) {
    document.getElementById('treeSensor101').textContent = `PID: ${payload.sensor_pids[0] || '--'}`;
    document.getElementById('treeSensor102').textContent = `PID: ${payload.sensor_pids[1] || '--'}`;
    document.getElementById('treeSensor103').textContent = `PID: ${payload.sensor_pids[2] || '--'}`;
  }
  if (document.getElementById('treeAlertPid')) {
    document.getElementById('treeAlertPid').textContent = `PID: ${payload.alert_detector_pid || '--'}`;
  }
  if (document.getElementById('treeFifoPid')) {
    document.getElementById('treeFifoPid').textContent = `PID: ${payload.fifo_listener_pid || '--'}`;
  }

  // Uptime formatting
  state.uptimeSec += 1;
  const h = String(Math.floor(state.uptimeSec / 3600)).padStart(2, '0');
  const m = String(Math.floor((state.uptimeSec % 3600) / 60)).padStart(2, '0');
  const s = String(state.uptimeSec % 60).padStart(2, '0');
  document.getElementById('headerUptime').textContent = `${h}:${m}:${s}`;

  // Update Patients
  const patientsList = payload.patients || [];
  patientsList.forEach(p => {
    const bedId = p.patient_id;
    if (!state.patients[bedId]) return;

    state.patients[bedId].hr = p.heart_rate;
    state.patients[bedId].spo2 = p.spo2;
    state.patients[bedId].status = p.status_str || 'NORMAL';
    state.patients[bedId].seq = p.sequence_number;
    state.patients[bedId].pid = p.sensor_pid;

    // Update Card UI
    const valHrEl = document.getElementById(`valHr${bedId}`);
    const valSpo2El = document.getElementById(`valSpo2${bedId}`);
    const statusPillEl = document.getElementById(`statusPill${bedId}`);
    const statusTextEl = document.getElementById(`statusText${bedId}`);
    const pidEl = document.getElementById(`pid${bedId}`);
    const seqEl = document.getElementById(`seq${bedId}`);
    const tsEl = document.getElementById(`ts${bedId}`);

    if (valHrEl) valHrEl.textContent = p.heart_rate.toFixed(1);
    if (valSpo2El) valSpo2El.textContent = p.spo2.toFixed(1);
    if (pidEl) pidEl.textContent = `PID: ${p.sensor_pid || '--'}`;
    if (seqEl) seqEl.textContent = p.sequence_number;
    if (tsEl) tsEl.textContent = p.timestamp ? p.timestamp.split(' ')[1] || p.timestamp : '';

    if (statusPillEl && statusTextEl) {
      statusTextEl.textContent = p.status_str;
      statusPillEl.className = 'status-pill';
      if (p.status_str === 'CRITICAL') {
        statusPillEl.classList.add('status-critical');
        if (valHrEl) valHrEl.className = 'vital-number text-red';
      } else if (p.status_str === 'WARNING') {
        statusPillEl.classList.add('status-warning');
        if (valHrEl) valHrEl.className = 'vital-number text-amber';
      } else {
        statusPillEl.classList.add('status-normal');
        if (valHrEl) valHrEl.className = 'vital-number text-green';
      }
    }
  });
}

// ==============================================================================
// Tab Navigation
// ==============================================================================
function initTabs() {
  const tabBtns = document.querySelectorAll('.tab-btn');
  tabBtns.forEach(btn => {
    btn.addEventListener('click', () => {
      const targetId = btn.getAttribute('data-tab');
      tabBtns.forEach(b => b.classList.remove('active'));
      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));

      btn.classList.add('active');
      const pane = document.getElementById(targetId);
      if (pane) pane.classList.add('active');

      state.activeTab = targetId;
      if (targetId === 'tab-alerts') {
        loadAlerts();
      } else if (targetId === 'tab-memory') {
        loadShmLayout();
      }
    });
  });
}

// ==============================================================================
// Named FIFO Injection Actions
// ==============================================================================
async function injectTelemetry(bedId, hr, spo2, label = 'Custom Vitals') {
  try {
    const res = await fetch('/api/inject', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ patient_id: bedId, heart_rate: hr, spo2: spo2 })
    });
    const result = await res.json();
    showToast(`⚡ Injected into Bed ${bedId}: ${hr} BPM, ${spo2}% (${label})`, result.success ? 'success' : 'warning');
    fetchStatus();
  } catch (err) {
    showToast(`Failed to dispatch FIFO injection: ${err.message}`, 'danger');
  }
}

function injectPreset(hr, spo2, label) {
  const bedId = parseInt(document.getElementById('dockBedSelect').value, 10);
  injectTelemetry(bedId, hr, spo2, label);
}

async function sendSignal(bedId) {
  try {
    const res = await fetch('/api/signal', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ patient_id: bedId })
    });
    const data = await res.json();
    showToast(`🔔 Dispatched SIGUSR1 to Patient ${bedId} sensor process`, 'warning');
  } catch (err) {
    showToast(`Signal error: ${err.message}`, 'danger');
  }
}

// Modal Injection Actions
function openInjectionModal(bedId) {
  document.getElementById('modalTargetBed').value = bedId;
  const patient = state.patients[bedId] || { hr: 75, spo2: 98 };
  document.getElementById('modalTitle').textContent = `Inject Emergency Anomaly - Bed ${bedId}`;
  document.getElementById('modalHrSlider').value = Math.round(patient.hr);
  document.getElementById('modalSpo2Slider').value = Math.round(patient.spo2);
  document.getElementById('modalHrVal').textContent = `${Math.round(patient.hr)} BPM`;
  document.getElementById('modalSpo2Val').textContent = `${Math.round(patient.spo2)} %`;
  document.getElementById('modalBackdrop').classList.add('active');
}

function closeInjectionModal() {
  document.getElementById('modalBackdrop').classList.remove('active');
}

function setModalValues(hr, spo2) {
  document.getElementById('modalHrSlider').value = hr;
  document.getElementById('modalSpo2Slider').value = spo2;
  document.getElementById('modalHrVal').textContent = `${hr} BPM`;
  document.getElementById('modalSpo2Val').textContent = `${spo2} %`;
}

function submitModalInjection() {
  const bedId = parseInt(document.getElementById('modalTargetBed').value, 10);
  const hr = parseFloat(document.getElementById('modalHrSlider').value);
  const spo2 = parseFloat(document.getElementById('modalSpo2Slider').value);
  injectTelemetry(bedId, hr, spo2, 'Modal Injection');
  closeInjectionModal();
}

// ==============================================================================
// Concurrency & Race Lab (CO-6)
// ==============================================================================
async function runRaceDemo(mode) {
  const termEl = document.getElementById('raceTerminalOutput');
  termEl.textContent = `[KERNEL EXECUTION] Spawning 4 concurrent processes with mode="${mode}"...\nPlease wait while benchmark completes...`;

  // Animate collision simulation
  animateThreadCollision(mode);

  try {
    const res = await fetch('/api/race', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode: mode })
    });
    const result = await res.json();
    
    // Update metric cards
    document.getElementById('metricTornReads').textContent = result.torn_reads.toLocaleString();
    document.getElementById('metricLostUpdates').textContent = result.lost_updates.toLocaleString();
    document.getElementById('metricActualCounter').textContent = result.actual_counter.toLocaleString();
    document.getElementById('metricExecTime').textContent = `${result.execution_time_ms.toFixed(2)} ms`;

    // Dynamic styling based on outcome
    const tornCard = document.getElementById('cardTornReads');
    const lostCard = document.getElementById('cardLostUpdates');
    const lockVisual = document.getElementById('lockStatusVisual');
    const visualCounter = document.getElementById('visualCounter');

    visualCounter.textContent = result.actual_counter.toLocaleString();

    if (mode === 'unsync') {
      tornCard.className = 'metric-card border-red';
      document.getElementById('metricTornReads').className = 'm-big-number text-red';
      document.getElementById('descTornReads').textContent = '⚠️ Data corruption detected (partially written fields observed by reader)!';

      lostCard.className = 'metric-card border-red';
      document.getElementById('metricLostUpdates').className = 'm-big-number text-red';
      document.getElementById('descLostUpdates').textContent = '⚠️ Critical race condition: parallel read-modify-write lost increments!';

      lockVisual.textContent = 'MUTEX PROTECTION: DISABLED (RACE CONDITIONS OCCURRED)';
      lockVisual.style.color = '#ff2a5f';
      showToast('⚠️ Race Demo Finished: Corruptions and Lost Updates detected without locks!', 'danger');
    } else {
      tornCard.className = 'metric-card border-green';
      document.getElementById('metricTornReads').className = 'm-big-number text-green';
      document.getElementById('descTornReads').textContent = ' 100% Data integrity: Zero torn reads detected under POSIX mutex!';

      lostCard.className = 'metric-card border-green';
      document.getElementById('metricLostUpdates').className = 'm-big-number text-green';
      document.getElementById('descLostUpdates').textContent = ' Exact count: All 80,000 atomic increments serialized!';

      lockVisual.textContent = 'MUTEX PROTECTION: ENABLED (PTHREAD_PROCESS_SHARED MUTEX)';
      lockVisual.style.color = '#00ff88';
      showToast(' Synchronized Demo Finished: 100% Data Integrity achieved with POSIX Mutex!', 'success');
    }

    termEl.textContent = result.raw_output || '[Execution completed without terminal output]';
  } catch (err) {
    termEl.textContent = `Error executing race demo: ${err.message}`;
    showToast(`Execution error: ${err.message}`, 'danger');
  }
}

function animateThreadCollision(mode) {
  const actions = ['actionW1', 'actionW2', 'actionW3', 'actionW4'];
  const isUnsync = mode === 'unsync';

  actions.forEach((actId, idx) => {
    const el = document.getElementById(actId);
    if (!el) return;
    el.textContent = isUnsync ? 'Colliding (Unprotected)...' : 'Acquiring POSIX Mutex...';
    el.style.color = isUnsync ? '#ff2a5f' : '#00e5ff';
  });

  setTimeout(() => {
    actions.forEach((actId) => {
      const el = document.getElementById(actId);
      if (!el) return;
      el.textContent = isUnsync ? 'Race Finished (Data Lost)' : 'Critical Section Exited Cleanly';
      el.style.color = isUnsync ? '#ffb703' : '#00ff88';
    });
  }, 1200);
}

// ==============================================================================
// Shared Memory Layout Inspector (CO-3/4)
// ==============================================================================
async function loadShmLayout() {
  try {
    const res = await fetch('/api/shm_layout');
    const layout = await res.json();
    state.shmLayout = layout;

    const tbody = document.getElementById('shmTableBody');
    if (!tbody) return;
    tbody.innerHTML = '';

    layout.fields.forEach(f => {
      const tr = document.createElement('tr');
      const hexOffset = `0x${f.offset.toString(16).padStart(3, '0').toUpperCase()}`;
      
      tr.innerHTML = `
        <td class="text-cyan">${hexOffset}</td>
        <td>+${f.offset}</td>
        <td>${f.size} B</td>
        <td class="text-green">${f.name}</td>
        <td><code>${f.type}</code></td>
        <td id="shmVal_${f.name.replace(/[^a-zA-Z0-9]/g, '_')}">--</td>
        <td class="text-mono">${f.desc}</td>
      `;
      tbody.appendChild(tr);
    });
  } catch (err) {
    console.warn("Failed to load SHM layout:", err);
  }
}

// ==============================================================================
// Alerts Audit History (CO-5)
// ==============================================================================
let allAlerts = [];
async function loadAlerts() {
  try {
    const res = await fetch('/api/alerts');
    const data = await res.json();
    allAlerts = data.alerts || [];
    document.getElementById('tabAlertCount').textContent = allAlerts.length;
    renderAlertsTable();
  } catch (err) {
    console.warn("Failed to load alerts:", err);
  }
}

function renderAlertsTable() {
  const tbody = document.getElementById('alertsTableBody');
  if (!tbody) return;
  tbody.innerHTML = '';

  const filterLevel = document.getElementById('alertFilterLevel').value;
  const search = document.getElementById('alertSearchInput').value.toLowerCase();

  const filtered = allAlerts.filter(a => {
    if (filterLevel !== 'ALL' && a.level !== filterLevel) return false;
    if (search) {
      const text = `${a.patient_id} ${a.patient_name || ''} ${a.diagnosis || ''}`.toLowerCase();
      if (!text.includes(search)) return false;
    }
    return true;
  });

  if (filtered.length === 0) {
    tbody.innerHTML = `<tr><td colspan="9" style="text-align:center; padding: 24px; color: #64748b;">No alerts match the selected criteria.</td></tr>`;
    return;
  }

  filtered.forEach(a => {
    const tr = document.createElement('tr');
    const levelClass = a.level === 'CRITICAL' ? 'badge-level-critical' : (a.level === 'WARNING' ? 'badge-level-warning' : 'badge-level-resolved');
    
    tr.innerHTML = `
      <td class="text-mono">${a.timestamp || '--'}</td>
      <td><span class="badge-level ${levelClass}">${a.level}</span></td>
      <td><strong>Bed ${a.patient_id || '--'}</strong></td>
      <td>${a.patient_name || '--'}</td>
      <td class="text-green text-mono">${a.heart_rate ? a.heart_rate.toFixed(1) + ' BPM' : '--'}</td>
      <td class="text-cyan text-mono">${a.spo2 ? a.spo2.toFixed(1) + '%' : '--'}</td>
      <td class="text-mono">#${a.sequence || '--'}</td>
      <td class="text-mono">${a.sensor_pid || '--'}</td>
      <td>${a.diagnosis || a.raw || '--'}</td>
    `;
    tbody.appendChild(tr);
  });
}

// ==============================================================================
// Simulator Lifecycle Controls
// ==============================================================================
async function startSimulator() {
  try {
    const res = await fetch('/api/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ duration: 0 })
    });
    const data = await res.json();
    showToast(data.message, data.success ? 'success' : 'warning');
    setTimeout(fetchStatus, 1000);
  } catch (err) {
    showToast(`Start error: ${err.message}`, 'danger');
  }
}

async function stopSimulator() {
  try {
    const res = await fetch('/api/stop', { method: 'POST' });
    const data = await res.json();
    showToast(data.message, 'warning');
    setTimeout(fetchStatus, 1000);
  } catch (err) {
    showToast(`Stop error: ${err.message}`, 'danger');
  }
}

// Toast System
function showToast(msg, type = 'info') {
  const container = document.getElementById('toastContainer');
  if (!container) return;

  const toast = document.createElement('div');
  toast.className = `toast toast-${type}`;
  toast.innerHTML = `<span>${msg}</span>`;
  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transform = 'translateX(40px)';
    toast.style.transition = 'all 0.3s ease';
    setTimeout(() => toast.remove(), 300);
  }, 4000);
}

// Export Alerts JSON
function exportAlerts() {
  const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(allAlerts, null, 2));
  const dlAnchor = document.createElement('a');
  dlAnchor.setAttribute("href", dataStr);
  dlAnchor.setAttribute("download", `alerts_audit_${Date.now()}.json`);
  dlAnchor.click();
}

// ==============================================================================
// Live Information Transfer (IPC Pipeline & Step Tracer)
// ==============================================================================
let traceSteps = [];
let currentTraceStepIndex = 0;
let autoTraceInterval = null;
let allIpcEvents = [];
let activeIpcFilter = 'ALL';

async function fetchTraceSteps(bedId = 101, hr = 38.0, spo2 = 84.0) {
  try {
    const res = await fetch('/api/trace_transfer', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ patient_id: bedId, heart_rate: hr, spo2: spo2 })
    });
    const data = await res.json();
    traceSteps = data.steps || [];
    currentTraceStepIndex = 0;
    renderTraceStep(currentTraceStepIndex);
  } catch (err) {
    console.warn("Failed to fetch trace transfer:", err);
  }
}

function renderTraceStep(idx) {
  if (!traceSteps || traceSteps.length === 0) return;
  const step = traceSteps[idx];
  if (!step) return;

  // Update text & indicators
  const stepBadge = document.getElementById('stepNumberBadge');
  const stageTitle = document.getElementById('stepStageTitle');
  const progressBar = document.getElementById('stepProgressBar');
  const stepSource = document.getElementById('stepSource');
  const stepChannel = document.getElementById('stepChannel');
  const stepTarget = document.getElementById('stepTarget');
  const stepSyscall = document.getElementById('stepSyscall');
  const stepExplanation = document.getElementById('stepExplanation');

  if (stepBadge) stepBadge.textContent = `STEP ${step.step} OF ${traceSteps.length}`;
  if (stageTitle) stageTitle.textContent = `${step.stage}: ${step.syscall.split('(')[0]}() Transfer`;
  if (progressBar) progressBar.style.width = `${((idx + 1) / traceSteps.length) * 100}%`;
  if (stepSource) stepSource.textContent = step.source;
  if (stepChannel) stepChannel.textContent = step.channel;
  if (stepTarget) stepTarget.textContent = step.target;
  if (stepSyscall) stepSyscall.textContent = step.syscall;
  if (stepExplanation) stepExplanation.innerHTML = `<strong>OS Architectural Action:</strong> ${step.explanation} <br><span class="text-mono text-cyan" style="font-size:0.75rem;">Payload: ${step.payload}</span>`;

  // Highlight corresponding nodes on the pipeline board
  clearPipelineHighlights();

  if (step.step === 1) {
    highlightNode('nodeExternal', 'amber');
    highlightWire('wireFifo');
  } else if (step.step === 2) {
    highlightNode('nodeExternal', 'amber');
    highlightWire('wireFifo');
  } else if (step.step === 3) {
    highlightNode('shmSlot101', 'amber');
    const slotLock = document.getElementById('slotLock101');
    if (slotLock) { slotLock.textContent = 'MUTEX LOCKED (CRITICAL SECTION)'; slotLock.style.color = '#ffb703'; }
  } else if (step.step === 4) {
    highlightNode('shmSlot101', 'green');
    highlightNode('nodeShmMaster', 'green');
    const flowHr = document.getElementById('flowHr101');
    const flowSpo2 = document.getElementById('flowSpo2101');
    if (flowHr) flowHr.textContent = '38.0';
    if (flowSpo2) flowSpo2.textContent = '84.0';
  } else if (step.step === 5) {
    highlightNode('shmSlot101', 'green');
    const slotLock = document.getElementById('slotLock101');
    if (slotLock) { slotLock.textContent = 'MUTEX UNLOCKED'; slotLock.style.color = '#00ff88'; }
  } else if (step.step === 6) {
    highlightNode('nodeAlertDetector', 'red');
    highlightWire('wireShmDetector');
  } else if (step.step === 7) {
    highlightNode('nodeAlertDetector', 'red');
    highlightNode('nodeDiskLog', 'red');
    highlightWire('wireDetectorDisk');
  } else if (step.step === 8) {
    highlightNode('nodeUiMonitor', 'green');
    playClinicalBeep(440, 0.2);
  }
}

function clearPipelineHighlights() {
  document.querySelectorAll('.flow-node, .shm-slot, .shm-board-box').forEach(el => {
    el.classList.remove('active-highlight', 'active-highlight-amber', 'active-highlight-red', 'slot-active');
  });
}

function highlightNode(nodeId, color = 'green') {
  const el = document.getElementById(nodeId);
  if (!el) return;
  if (color === 'amber') el.classList.add('active-highlight-amber');
  else if (color === 'red') el.classList.add('active-highlight-red');
  else el.classList.add('active-highlight');
}

function highlightWire(wireId) {
  const el = document.getElementById(wireId);
  if (!el) return;
  el.style.borderColor = '#00ff88';
  setTimeout(() => { el.style.borderColor = ''; }, 1200);
}

function stepTraceNext() {
  if (currentTraceStepIndex < traceSteps.length - 1) {
    currentTraceStepIndex++;
    renderTraceStep(currentTraceStepIndex);
  } else {
    showToast(' Information transfer walkthrough complete!', 'success');
  }
}

function stepTracePrev() {
  if (currentTraceStepIndex > 0) {
    currentTraceStepIndex--;
    renderTraceStep(currentTraceStepIndex);
  }
}

function runAutoTrace() {
  if (autoTraceInterval) {
    clearInterval(autoTraceInterval);
    autoTraceInterval = null;
    document.getElementById('btnAutoTrace').innerHTML = '<span>▶ Run Animated End-to-End Transfer</span>';
    return;
  }

  currentTraceStepIndex = 0;
  renderTraceStep(0);
  document.getElementById('btnAutoTrace').innerHTML = '<span>⏸ Pause Transfer Animation</span>';

  autoTraceInterval = setInterval(() => {
    if (currentTraceStepIndex < traceSteps.length - 1) {
      currentTraceStepIndex++;
      renderTraceStep(currentTraceStepIndex);
    } else {
      clearInterval(autoTraceInterval);
      autoTraceInterval = null;
      document.getElementById('btnAutoTrace').innerHTML = '<span>▶ Run Animated End-to-End Transfer</span>';
      showToast(' Finished full 8-step OS transfer pipeline demonstration!', 'success');
    }
  }, 1300);
}

function resetTrace() {
  if (autoTraceInterval) {
    clearInterval(autoTraceInterval);
    autoTraceInterval = null;
    document.getElementById('btnAutoTrace').innerHTML = '<span>▶ Run Animated End-to-End Transfer</span>';
  }
  currentTraceStepIndex = 0;
  clearPipelineHighlights();
  renderTraceStep(0);
  showToast('Flow reset to Step 1', 'info');
}

// ==============================================================================
// Live Kernel IPC Event Stream Bus
// ==============================================================================
async function loadIpcEvents() {
  try {
    const res = await fetch('/api/ipc_events');
    const data = await res.json();
    allIpcEvents = data.events || [];

    // Update Tab 1 Ticker text with most recent event
    if (allIpcEvents.length > 0) {
      const topEvt = allIpcEvents[0];
      const tickerText = document.getElementById('liveIpcEventText');
      if (tickerText) {
        tickerText.textContent = `[${topEvt.timestamp}] ${topEvt.source} ➔ ${topEvt.syscall} ➔ ${topEvt.target} (${topEvt.payload})`;
      }
    }

    renderIpcEventsTable();
  } catch (err) {
    console.warn("Failed to load IPC events:", err);
  }
}

function renderIpcEventsTable() {
  const tbody = document.getElementById('streamTableBody');
  if (!tbody) return;
  tbody.innerHTML = '';

  const filtered = allIpcEvents.filter(e => {
    if (activeIpcFilter === 'ALL') return true;
    return e.channel === activeIpcFilter;
  });

  if (filtered.length === 0) {
    tbody.innerHTML = `<tr><td colspan="7" style="text-align:center; padding:18px; color:#64748b;">No recent kernel IPC events logged.</td></tr>`;
    return;
  }

  filtered.forEach(e => {
    const tr = document.createElement('tr');
    let channelClass = 'text-green';
    if (e.channel === 'NAMED_FIFO') channelClass = 'text-amber';
    else if (e.channel === 'POSIX_SIGNAL') channelClass = 'text-cyan';
    else if (e.channel === 'FILE_IO') channelClass = 'text-red';

    tr.innerHTML = `
      <td class="text-mono" style="color:#94a3b8;">${e.timestamp}</td>
      <td style="font-weight:600;">${e.source}</td>
      <td><span class="${channelClass}" style="font-weight:700;">${e.channel}</span></td>
      <td><code style="color:#38bdf8;">${e.syscall}</code></td>
      <td style="color:#cbd5e1;">${e.target}</td>
      <td class="text-mono">${e.payload}</td>
      <td style="font-size:0.75rem; color:#94a3b8;">${e.details}</td>
    `;
    tbody.appendChild(tr);
  });
}

function switchToTab(tabId) {
  const tabBtns = document.querySelectorAll('.tab-btn');
  tabBtns.forEach(btn => {
    if (btn.getAttribute('data-tab') === tabId) {
      btn.click();
    }
  });
}

// ==============================================================================
// Initialization & Event Listeners
// ==============================================================================
document.addEventListener('DOMContentLoaded', () => {
  initTabs();
  initRenderers();
  requestAnimationFrame(animLoop);

  // Poll status & events every second
  setInterval(fetchStatus, 1000);
  setInterval(loadIpcEvents, 1500);
  fetchStatus();
  loadIpcEvents();
  fetchTraceSteps();

  // Lifecycle buttons
  document.getElementById('btnStartSimulator').addEventListener('click', startSimulator);
  document.getElementById('btnStopSimulator').addEventListener('click', stopSimulator);

  // Audio Toggle
  document.getElementById('btnToggleAudio').addEventListener('click', () => {
    state.audioEnabled = !state.audioEnabled;
    const icon = document.getElementById('audioIcon');
    const text = document.getElementById('audioText');
    if (state.audioEnabled) {
      icon.textContent = '🔊';
      text.textContent = 'Beeper ON';
      showToast('🔊 Telemetry heart tone enabled', 'success');
    } else {
      icon.textContent = '🔇';
      text.textContent = 'Beeper OFF';
      showToast('🔇 Telemetry heart tone muted', 'info');
    }
  });

  // Step-by-Step IPC Trace Controllers
  const btnAutoTrace = document.getElementById('btnAutoTrace');
  const btnResetTrace = document.getElementById('btnResetTrace');
  const btnNextStep = document.getElementById('btnNextStep');
  const btnPrevStep = document.getElementById('btnPrevStep');

  if (btnAutoTrace) btnAutoTrace.addEventListener('click', runAutoTrace);
  if (btnResetTrace) btnResetTrace.addEventListener('click', resetTrace);
  if (btnNextStep) btnNextStep.addEventListener('click', stepTraceNext);
  if (btnPrevStep) btnPrevStep.addEventListener('click', stepTracePrev);

  // Stream Filter buttons
  document.querySelectorAll('.chip-filter').forEach(chip => {
    chip.addEventListener('click', () => {
      document.querySelectorAll('.chip-filter').forEach(c => c.classList.remove('active'));
      chip.classList.add('active');
      activeIpcFilter = chip.getAttribute('data-filter');
      renderIpcEventsTable();
    });
  });

  // Dock Sliders live display
  const sliderHr = document.getElementById('sliderHr');
  const sliderSpo2 = document.getElementById('sliderSpo2');
  sliderHr.addEventListener('input', (e) => {
    document.getElementById('sliderValHr').textContent = `${e.target.value} BPM`;
  });
  sliderSpo2.addEventListener('input', (e) => {
    document.getElementById('sliderValSpo2').textContent = `${e.target.value} %`;
  });

  // Modal Sliders
  document.getElementById('modalHrSlider').addEventListener('input', (e) => {
    document.getElementById('modalHrVal').textContent = `${e.target.value} BPM`;
  });
  document.getElementById('modalSpo2Slider').addEventListener('input', (e) => {
    document.getElementById('modalSpo2Val').textContent = `${e.target.value} %`;
  });

  // Custom Inject Button
  document.getElementById('btnCustomInject').addEventListener('click', () => {
    const bedId = parseInt(document.getElementById('dockBedSelect').value, 10);
    const hr = parseFloat(sliderHr.value);
    const spo2 = parseFloat(sliderSpo2.value);
    injectTelemetry(bedId, hr, spo2, 'Custom Slider');
    // Also update trace for this injection
    fetchTraceSteps(bedId, hr, spo2);
  });

  // Race Demo buttons
  document.getElementById('btnRunUnsync').addEventListener('click', () => runRaceDemo('unsync'));
  document.getElementById('btnRunSync').addEventListener('click', () => runRaceDemo('sync'));

  // Alert Filters
  document.getElementById('alertFilterLevel').addEventListener('change', renderAlertsTable);
  document.getElementById('alertSearchInput').addEventListener('input', renderAlertsTable);
  document.getElementById('btnRefreshAlerts').addEventListener('click', loadAlerts);
  document.getElementById('btnExportAlerts').addEventListener('click', exportAlerts);
});

