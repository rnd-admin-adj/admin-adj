const SERVER_URL = window.location.origin;

const dropZone    = document.getElementById('dropZone');
const fileInput   = document.getElementById('fileInput');
const progressCard = document.getElementById('progressCard');
const summaryCard  = document.getElementById('summaryCard');
const progressBar  = document.getElementById('progressBar');
const progressLabel = document.getElementById('progressLabel');
const progressTitle = document.getElementById('progressTitle');
const progressIcon  = document.getElementById('progressIcon');
const logBox       = document.getElementById('logBox');

// ── Drag & drop wiring ────────────────────────────────────────────────────
dropZone.addEventListener('dragover', e => { e.preventDefault(); dropZone.classList.add('drag-over'); });
dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag-over'));
dropZone.addEventListener('drop', e => {
    e.preventDefault();
    dropZone.classList.remove('drag-over');
    const file = e.dataTransfer.files[0];
    if (file) startUpload(file);
});
fileInput.addEventListener('change', () => {
    if (fileInput.files[0]) startUpload(fileInput.files[0]);
});

function log(msg, cls = '') {
    const line = document.createElement('div');
    if (cls) line.className = cls;
    line.textContent = msg;
    logBox.appendChild(line);
    logBox.scrollTop = logBox.scrollHeight;
}

function setProgress(pct, label) {
    progressBar.style.width = pct + '%';
    progressLabel.textContent = label;
}

// ── Upload & parse ────────────────────────────────────────────────────────
async function startUpload(file) {
    dropZone.style.display    = 'none';
    progressCard.style.display = 'block';
    summaryCard.style.display  = 'none';
    logBox.innerHTML = '';

    log(`File: ${file.name}  (${(file.size / 1024).toFixed(1)} KB)`, 'info');
    setProgress(10, 'Uploading…');

    const form = new FormData();
    form.append('logfile', file);

    let result;
    try {
        const res = await fetch(`${SERVER_URL}/api/import/sdcard`, {
            method: 'POST',
            body: form,
        });
        result = await res.json();
    } catch (e) {
        log('Upload failed: ' + e.message, 'err');
        setProgress(0, 'Upload failed');
        return;
    }

    if (result.error) {
        log('Server error: ' + result.error, 'err');
        setProgress(0, 'Import failed');
        return;
    }

    setProgress(100, 'Done');
    progressIcon.className = 'fas fa-check-circle';
    progressIcon.style.color = '#22c55e';
    progressTitle.textContent = 'Import complete';

    log(`File: ${result.totalRecords} records × 162 bytes = ${result.bytesScanned} bytes`, 'info');
    log(`Sensor packets  : ${result.sensorInserted} inserted`, 'ok');
    log(`No GPS lock     : ${result.noGpsSkipped} packets skipped (yr=0)`, result.noGpsSkipped > 0 ? 'warn' : 'ok');
    log(`CRC failures    : ${result.crcFailed}`, result.crcFailed > 0 ? 'err' : 'ok');
    log(`Event packets   : ${result.eventInserted} inserted`, 'ok');
    log(`GPS points      : ${result.gpsInserted}`, 'info');
    log(`Impact events   : ${result.impactsDetected}`, result.impactsDetected > 0 ? 'warn' : 'ok');
    if (result.fromTime) log(`Time range      : ${result.fromTime}  →  ${result.toTime}`, 'info');

    // Fill summary card
    document.getElementById('statTotal').textContent   = result.sensorInserted + result.eventInserted;
    document.getElementById('statSensor').textContent  = result.sensorInserted;
    document.getElementById('statEvents').textContent  = result.impactsDetected;
    document.getElementById('statGps').textContent     = result.gpsInserted;
    document.getElementById('statDropped').textContent = result.noGpsSkipped ?? result.sensorDropped;

    const tr = document.getElementById('timeRange');
    if (result.fromTime && result.toTime) {
        tr.innerHTML = `<i class="fas fa-clock"></i>  Data range: <strong>${result.fromTime}</strong> &rarr; <strong>${result.toTime}</strong>`;
    } else {
        tr.textContent = 'No valid GPS timestamps found in file.';
    }

    // Store range so graphs/events pages can auto-load it
    if (result.fromTime && result.toTime) {
        _importRange = { from: result.fromTime, to: result.toTime };
    }

    summaryCard.style.display = 'block';
}

// ── Navigation helpers ────────────────────────────────────────────────────
let _importRange = null;

function goToGraphs() {
    if (_importRange) localStorage.setItem('sdcard_import_range', JSON.stringify(_importRange));
    if (window.parent && window.parent.loadPage) {
        window.parent.loadPage('pages/graphs.html');
    } else {
        window.location.href = '../index.html';
    }
}

function goToEvents() {
    if (_importRange) localStorage.setItem('sdcard_import_range', JSON.stringify(_importRange));
    if (window.parent && window.parent.loadPage) {
        window.parent.loadPage('pages/events.html');
    } else {
        window.location.href = '../index.html';
    }
}

function resetPage() {
    dropZone.style.display     = 'block';
    progressCard.style.display = 'none';
    summaryCard.style.display  = 'none';
    fileInput.value = '';
    progressBar.style.width = '0%';
    progressIcon.className  = 'fas fa-cog fa-spin';
    progressIcon.style.color = '';
    progressTitle.textContent = 'Parsing…';
    logBox.innerHTML = '';
}
