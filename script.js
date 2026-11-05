(() => {
  'use strict';

  const POLL_INTERVAL_MS = 1000;
  const ADC_MAX = 4095;
  const byId = (id) => document.getElementById(id);
  const ui = {
    address: byId('deviceAddress'), connect: byId('connectButton'), connection: document.querySelector('.connection'),
    connectionLabel: byId('connectionLabel'), connectionDetail: byId('connectionDetail'), network: byId('networkStatus'),
    updated: byId('updatedAt'), panel: byId('statusPanel'), eyebrow: byId('statusEyebrow'), title: byId('statusTitle'),
    message: byId('statusMessage'), mode: byId('operatingMode'), warmup: byId('warmupState'),
    reading: byId('readingValue'), threshold: byId('thresholdValue'), delta: byId('deltaValue'), gauge: byId('gauge'),
    fill: byId('gaugeFill'), marker: byId('thresholdMarker'), gaugeThreshold: byId('gaugeThreshold'),
    green: byId('greenIndicator'), red: byId('redIndicator'), buzzer: byId('buzzerIndicator'),
    greenState: byId('greenState'), redState: byId('redState'), buzzerState: byId('buzzerState'),
    sound: byId('soundToggle'), soundState: byId('soundState'), test: byId('testButton'), testState: byId('testState')
  };

  let baseUrl = '';
  let soundEnabled = false;
  let audioContext = null;
  let sirenMaster = null;
  let sirenOscillators = [];
  let sirenScheduler = null;
  let sirenActive = false;
  let sirenHighTone = false;
  let latestData = null;
  let polling = false;

  try {
    const saved = localStorage.getItem('gasMonitorEsp32Address');
    if (saved) ui.address.value = saved;
  } catch (_) { /* Storage may be unavailable in private browsing. */ }

  function normalizeAddress(value) {
    let parsed;
    try { parsed = new URL(value.trim()); } catch (_) { throw new Error('Enter the ESP32 IP address, including http://.'); }
    if (!['http:', 'https:'].includes(parsed.protocol)) throw new Error('Use an http:// or https:// address.');
    return parsed.origin;
  }

  function setConnection(connected, detail) {
    ui.connection.classList.toggle('online', connected);
    ui.connection.classList.toggle('offline', !connected);
    ui.connectionLabel.textContent = connected ? 'ESP32 connected' : (baseUrl ? 'ESP32 unavailable' : 'Not connected');
    ui.connectionDetail.textContent = connected ? 'Live data stream active' : (baseUrl ? 'Checking device connection' : 'Enter device address to begin');
    ui.network.textContent = detail;
    ui.test.disabled = !connected;
  }

  function showWaiting(message) {
    ui.panel.className = 'status-panel status-warming';
    ui.eyebrow.textContent = 'SYSTEM STATUS';
    ui.title.textContent = 'Awaiting sensor data';
    ui.message.textContent = message;
    ui.mode.textContent = '—';
    ui.warmup.textContent = '—';
    ui.reading.textContent = '—';
    ui.threshold.textContent = '—';
    ui.delta.textContent = '—';
    ui.gaugeThreshold.textContent = '—';
    ui.fill.style.width = '0%';
    ui.marker.style.left = '0%';
    ui.gauge.setAttribute('aria-valuenow', '0');
    ui.green.classList.remove('is-on');
    ui.red.classList.remove('is-on');
    ui.buzzer.classList.remove('is-active');
    ui.greenState.textContent = 'OFF';
    ui.redState.textContent = 'OFF';
    ui.buzzerState.textContent = 'INACTIVE';
    latestData = null;
    syncSiren();
  }

  function render(data) {
    const raw = Number(data.raw);
    const threshold = Number(data.threshold);
    if (!Number.isFinite(raw) || !Number.isFinite(threshold)) throw new Error('The ESP32 returned an unexpected sensor response.');

    const alert = data.alert === true;
    const warming = data.warming === true;
    const simulated = data.sim === true;
    const left = Math.max(0, Number(data.left) || 0);
    const difference = raw - threshold;
    const percent = Math.max(0, Math.min(100, (raw / ADC_MAX) * 100));
    const thresholdPercent = Math.max(0, Math.min(100, (threshold / ADC_MAX) * 100));
    latestData = { alert, warming };

    ui.panel.className = `status-panel ${alert ? 'status-warning' : (warming ? 'status-warming' : 'status-safe')}`;
    ui.eyebrow.textContent = alert ? 'HAZARD DETECTED' : (warming ? 'INITIALIZATION' : 'SYSTEM STATUS');
    ui.title.textContent = alert ? 'WARNING — elevated gas / smoke' : (warming ? 'Sensor warming up' : 'SAFE / NORMAL');
    ui.message.textContent = alert
      ? 'MQ-2 reading is at or above the configured alarm threshold. Check the monitored area.'
      : (warming ? `MQ-2 sensor warm-up in progress · approximately ${left} seconds remaining.` : 'Gas / smoke level is below the configured threshold.');
    ui.mode.textContent = simulated ? 'TEST SIMULATION' : 'REAL-TIME';
    ui.warmup.textContent = warming ? `${left}s REMAINING` : 'COMPLETE';

    ui.reading.textContent = String(raw);
    ui.threshold.textContent = String(threshold);
    ui.delta.textContent = `${difference > 0 ? '+' : ''}${difference} counts`;
    ui.delta.style.color = alert ? 'var(--red)' : '';
    ui.gaugeThreshold.textContent = String(threshold);
    ui.fill.style.width = `${percent}%`;
    ui.marker.style.left = `${thresholdPercent}%`;
    ui.gauge.classList.toggle('warning', alert);
    ui.gauge.setAttribute('aria-valuenow', String(Math.max(0, Math.min(ADC_MAX, raw))));

    ui.green.classList.toggle('is-on', !alert && !warming);
    ui.red.classList.toggle('is-on', alert);
    ui.greenState.textContent = !alert && !warming ? 'ON' : 'OFF';
    ui.redState.textContent = alert ? 'ON' : 'OFF';
    ui.buzzer.classList.toggle('is-active', alert);
    ui.buzzerState.textContent = alert ? 'ACTIVE' : 'INACTIVE';
    ui.test.setAttribute('aria-pressed', String(simulated));
    ui.test.textContent = simulated ? 'Stop test' : 'Start test';
    ui.testState.textContent = simulated ? 'ON' : 'OFF';
    ui.updated.textContent = new Intl.DateTimeFormat(undefined, { hour: 'numeric', minute: '2-digit', second: '2-digit' }).format(new Date());
    syncSiren();
  }

  async function fetchData() {
    if (!baseUrl || polling) return;
    polling = true;
    try {
      const response = await fetch(`${baseUrl}/data`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`ESP32 responded with HTTP ${response.status}.`);
      const data = await response.json();
      render(data);
      setConnection(true, 'Device API reachable · polling every second');
    } catch (error) {
      setConnection(false, error instanceof TypeError ? 'Could not reach the device. Check its IP and network.' : error.message);
    } finally {
      polling = false;
    }
  }

  function ensureSirenEngine() {
    if (audioContext) return true;
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) {
      ui.network.textContent = 'This browser does not support Web Audio.';
      return false;
    }

    try {
      audioContext = new AudioContextClass();
      const compressor = audioContext.createDynamicsCompressor();
      compressor.threshold.setValueAtTime(-8, audioContext.currentTime);
      compressor.knee.setValueAtTime(6, audioContext.currentTime);
      compressor.ratio.setValueAtTime(12, audioContext.currentTime);
      compressor.attack.setValueAtTime(0.004, audioContext.currentTime);
      compressor.release.setValueAtTime(0.12, audioContext.currentTime);

      sirenMaster = audioContext.createGain();
      sirenMaster.gain.setValueAtTime(0.0001, audioContext.currentTime);
      sirenMaster.connect(compressor);
      compressor.connect(audioContext.destination);

      // Keep one pair of oscillators alive and muted between alarms. Polling only
      // changes the master gain; it never creates more oscillators.
      const fundamentalGain = audioContext.createGain();
      fundamentalGain.gain.setValueAtTime(0.72, audioContext.currentTime);
      const harmonicGain = audioContext.createGain();
      harmonicGain.gain.setValueAtTime(0.12, audioContext.currentTime);

      const fundamental = audioContext.createOscillator();
      fundamental.type = 'sawtooth';
      fundamental.frequency.setValueAtTime(520, audioContext.currentTime);
      fundamental.connect(fundamentalGain);
      fundamentalGain.connect(sirenMaster);
      fundamental.start();

      const harmonic = audioContext.createOscillator();
      harmonic.type = 'square';
      harmonic.frequency.setValueAtTime(1040, audioContext.currentTime);
      harmonic.connect(harmonicGain);
      harmonicGain.connect(sirenMaster);
      harmonic.start();

      sirenOscillators = [fundamental, harmonic];
      audioContext.addEventListener('statechange', () => {
        if (audioContext.state === 'running') syncSiren();
      });
      return true;
    } catch (error) {
      audioContext = null;
      sirenMaster = null;
      sirenOscillators = [];
      ui.network.textContent = `Unable to initialize browser audio: ${error.message}`;
      return false;
    }
  }

  function setSirenLevel(level) {
    if (!audioContext || !sirenMaster) return;
    const now = audioContext.currentTime;
    const parameter = sirenMaster.gain;
    parameter.cancelScheduledValues(now);
    parameter.setTargetAtTime(level, now, level > 0.001 ? 0.035 : 0.012);
  }

  function setSirenPitch(high) {
    if (!audioContext || sirenOscillators.length !== 2) return;
    const now = audioContext.currentTime;
    const baseHz = high ? 880 : 520;
    sirenOscillators[0].frequency.cancelScheduledValues(now);
    sirenOscillators[0].frequency.setTargetAtTime(baseHz, now, 0.025);
    sirenOscillators[1].frequency.cancelScheduledValues(now);
    sirenOscillators[1].frequency.setTargetAtTime(baseHz * 2, now, 0.025);
  }

  function activateAudioFromGesture() {
    if (!ensureSirenEngine()) return;
    if (audioContext.state === 'suspended') {
      audioContext.resume().then(() => {
        syncSiren();
      }).catch(() => {
        ui.network.textContent = 'Audio is paused by the browser. Use the Sound control to resume it.';
      });
    }
  }

  function startSiren() {
    if (sirenActive || !audioContext || audioContext.state !== 'running') return;
    sirenActive = true;
    sirenHighTone = false;
    setSirenPitch(sirenHighTone);
    setSirenLevel(0.58);
    sirenScheduler = window.setInterval(() => {
      if (!sirenActive || !audioContext || audioContext.state !== 'running') return;
      sirenHighTone = !sirenHighTone;
      setSirenPitch(sirenHighTone);
    }, 420);
  }

  function stopSiren() {
    if (sirenScheduler) window.clearInterval(sirenScheduler);
    sirenScheduler = null;
    sirenActive = false;
    setSirenLevel(0.0001);
  }

  function syncSiren() {
    const warningActive = Boolean(latestData && latestData.alert);
    if (soundEnabled && warningActive) startSiren();
    else stopSiren();
  }

  function connect() {
    try {
      baseUrl = normalizeAddress(ui.address.value);
      ui.address.value = baseUrl;
      try { localStorage.setItem('gasMonitorEsp32Address', baseUrl); } catch (_) { /* Optional preference. */ }
      setConnection(false, 'Connecting to the ESP32 data endpoint…');
      fetchData();
    } catch (error) {
      setConnection(false, error.message);
    }
  }

  ui.connect.addEventListener('click', connect);
  ui.address.addEventListener('keydown', (event) => { if (event.key === 'Enter') connect(); });
  ui.sound.addEventListener('click', () => {
    soundEnabled = !soundEnabled;
    ui.sound.setAttribute('aria-checked', String(soundEnabled));
    ui.soundState.textContent = soundEnabled ? 'ENABLED' : 'MUTED';
    if (soundEnabled) activateAudioFromGesture();
    syncSiren();
  });
  ui.test.addEventListener('click', async () => {
    if (!baseUrl) return;
    activateAudioFromGesture();
    const nextState = ui.test.getAttribute('aria-pressed') !== 'true';
    ui.test.disabled = true;
    try {
      const response = await fetch(`${baseUrl}/sim?on=${nextState ? '1' : '0'}`, { cache: 'no-store' });
      if (!response.ok) throw new Error(`Test control failed (HTTP ${response.status}).`);
      await fetchData();
    } catch (error) {
      ui.network.textContent = error instanceof TypeError ? 'Could not reach the test control endpoint.' : error.message;
    } finally {
      ui.test.disabled = !ui.connection.classList.contains('online');
    }
  });

  setConnection(false, 'Enter the IP shown in the ESP32 Serial Monitor');
  showWaiting('Connect to the ESP32 to view live MQ-2 readings.');
  window.setInterval(fetchData, POLL_INTERVAL_MS);
  if (ui.address.value.trim()) connect();
})();
