/**
 * FacesView — Face Library management page for LightNVR.
 * Inspired by Frigate's face recognition interface.
 *
 * Features:
 *  - View all trained face profiles as photo-cards
 *  - Train a new face via drag-and-drop or file picker
 *  - Delete individual faces from the library
 *  - Test recognition by uploading a photo
 */

import { useState, useRef, useCallback, useEffect } from 'preact/hooks';

const FACE_API_BASE = '/api/faces';

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

async function apiFetch(path, options = {}) {
  const res = await fetch(path, options);
  if (!res.ok) {
    const text = await res.text().catch(() => res.statusText);
    throw new Error(`${res.status}: ${text}`);
  }
  return res.json();
}

function readFileAsDataURL(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (e) => resolve(e.target.result);
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

function formatFaceDate(value) {
  if (!value) return '';
  const normalized = typeof value === 'string' && value.includes(' ')
    ? `${value.replace(' ', 'T')}Z`
    : value;
  const date = new Date(normalized);
  if (Number.isNaN(date.getTime())) return '';
  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  }).format(date);
}

// ─────────────────────────────────────────────────────────────────────────────
// DragDrop upload zone
// ─────────────────────────────────────────────────────────────────────────────

function DropZone({ onFile, accept = 'image/*', label = 'Drop image here or click to browse' }) {
  const [over, setOver] = useState(false);
  const inputRef = useRef(null);

  const handleDrop = useCallback((e) => {
    e.preventDefault();
    setOver(false);
    const file = e.dataTransfer?.files?.[0];
    if (file) onFile(file);
  }, [onFile]);

  const handleDragOver = (e) => { e.preventDefault(); setOver(true); };
  const handleDragLeave = () => setOver(false);

  return (
    <div
      onClick={() => inputRef.current?.click()}
      onDrop={handleDrop}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      className={`flex flex-col items-center justify-center gap-3 rounded-xl border-2 border-dashed cursor-pointer transition-all p-6 min-h-[140px] ${
        over
          ? 'border-primary bg-primary/10 scale-[1.01]'
          : 'border-border bg-card hover:border-primary/60 hover:bg-muted/40'
      }`}
    >
      <svg className="w-10 h-10 text-muted-foreground" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5}
          d="M12 16v-8m0 0-3 3m3-3 3 3M4 16v1a3 3 0 003 3h10a3 3 0 003-3v-1" />
      </svg>
      <span className="text-sm text-muted-foreground text-center">{label}</span>
      <input
        ref={inputRef}
        type="file"
        accept={accept}
        className="hidden"
        onChange={(e) => { const f = e.target.files?.[0]; if (f) onFile(f); e.target.value = ''; }}
      />
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Train Face Panel
// ─────────────────────────────────────────────────────────────────────────────

function TrainFacePanel({ onTrained }) {
  const [name, setName] = useState('');
  const [preview, setPreview] = useState(null);
  const [file, setFile] = useState(null);
  const [status, setStatus] = useState(null); // {type:'success'|'error', msg}
  const [busy, setBusy] = useState(false);

  const handleFile = useCallback(async (f) => {
    setFile(f);
    setStatus(null);
    const dataUrl = await readFileAsDataURL(f);
    setPreview(dataUrl);
  }, []);

  const handleSubmit = async (e) => {
    e.preventDefault();
    if (!file || !name.trim()) return;
    setBusy(true);
    setStatus(null);
    try {
      const fd = new FormData();
      fd.append('name', name.trim());
      fd.append('file', file);
      const resp = await fetch(`${FACE_API_BASE}/train`, { method: 'POST', body: fd });
      const data = await resp.json();
      if (!resp.ok || !data.success) throw new Error(data.detail || data.message || 'Training failed');
      setStatus({ type: 'success', msg: `✓ Trained face for "${data.name}"` });
      setName('');
      setFile(null);
      setPreview(null);
      onTrained?.();
    } catch (err) {
      setStatus({ type: 'error', msg: `✗ ${err.message}` });
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="bg-card rounded-2xl border border-border shadow-sm p-5 flex flex-col gap-4">
      <h3 className="text-base font-semibold flex items-center gap-2">
        <svg className="w-5 h-5 text-primary" fill="none" viewBox="0 0 24 24" stroke="currentColor">
          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 4v16m8-8H4" />
        </svg>
        Train New Face
      </h3>

      <form onSubmit={handleSubmit} className="flex flex-col gap-3">
        <input
          type="text"
          placeholder="Person's name (e.g. Alice)"
          className="input input-bordered w-full"
          value={name}
          onInput={(e) => setName(e.target.value)}
          required
          maxLength={64}
        />

        {preview ? (
          <div className="relative">
            <img
              src={preview}
              alt="Preview"
              className="w-full max-h-52 rounded-xl object-cover border border-border"
            />
            <button
              type="button"
              onClick={() => { setFile(null); setPreview(null); }}
              className="absolute top-2 right-2 p-1 rounded-full bg-background/80 backdrop-blur border border-border hover:bg-destructive hover:text-destructive-foreground transition-colors"
              title="Remove image"
            >
              <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
              </svg>
            </button>
          </div>
        ) : (
          <DropZone onFile={handleFile} label="Drop a clear face photo here or click to browse" />
        )}

        <button
          type="submit"
          disabled={!file || !name.trim() || busy}
          className="btn-primary w-full disabled:opacity-50 disabled:cursor-not-allowed"
        >
          {busy ? 'Training…' : 'Train Face'}
        </button>

        {status && (
          <p className={`text-sm rounded-lg px-3 py-2 ${
            status.type === 'success'
              ? 'bg-green-500/10 text-green-600 dark:text-green-400 border border-green-500/20'
              : 'bg-destructive/10 text-destructive border border-destructive/20'
          }`}>
            {status.msg}
          </p>
        )}
      </form>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Test Recognition Panel
// ─────────────────────────────────────────────────────────────────────────────

function TestRecognitionPanel() {
  const [preview, setPreview] = useState(null);
  const [file, setFile] = useState(null);
  const [result, setResult] = useState(null);
  const [busy, setBusy] = useState(false);

  const handleFile = useCallback(async (f) => {
    setFile(f);
    setResult(null);
    const dataUrl = await readFileAsDataURL(f);
    setPreview(dataUrl);
  }, []);

  const handleTest = async () => {
    if (!file) return;
    setBusy(true);
    setResult(null);
    try {
      const fd = new FormData();
      fd.append('file', file);
      const resp = await fetch(`${FACE_API_BASE}/recognize`, { method: 'POST', body: fd });
      const data = await resp.json();
      if (!resp.ok) throw new Error(data.detail || 'Recognition failed');
      setResult(data);
    } catch (err) {
      setResult({ success: false, name: 'Error', confidence: 0, error: err.message });
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="bg-card rounded-2xl border border-border shadow-sm p-5 flex flex-col gap-4">
      <h3 className="text-base font-semibold flex items-center gap-2">
        <svg className="w-5 h-5 text-primary" fill="none" viewBox="0 0 24 24" stroke="currentColor">
          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2}
            d="M15 12a3 3 0 11-6 0 3 3 0 016 0z" />
          <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2}
            d="M2.458 12C3.732 7.943 7.523 5 12 5c4.478 0 8.268 2.943 9.542 7-1.274 4.057-5.064 7-9.542 7-4.477 0-8.268-2.943-9.542-7z" />
        </svg>
        Test Recognition
      </h3>

      {preview ? (
        <div className="relative">
          <img
            src={preview}
            alt="Test preview"
            className="w-full max-h-52 rounded-xl object-cover border border-border"
          />
          <button
            type="button"
            onClick={() => { setFile(null); setPreview(null); setResult(null); }}
            className="absolute top-2 right-2 p-1 rounded-full bg-background/80 backdrop-blur border border-border hover:bg-destructive hover:text-destructive-foreground transition-colors"
          >
            <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M6 18L18 6M6 6l12 12" />
            </svg>
          </button>
        </div>
      ) : (
        <DropZone onFile={handleFile} label="Drop a photo to test recognition" />
      )}

      {file && (
        <button
          onClick={handleTest}
          disabled={busy}
          className="btn-primary w-full disabled:opacity-50"
        >
          {busy ? 'Recognizing…' : 'Recognize Face'}
        </button>
      )}

      {result && (
        <div className={`rounded-xl p-4 border ${
          result.success
            ? 'bg-green-500/10 border-green-500/30'
            : 'bg-muted border-border'
        }`}>
          {result.success ? (
            <>
              <div className="flex items-center gap-3">
                <div className="w-10 h-10 rounded-full bg-primary flex items-center justify-center text-primary-foreground font-bold text-lg">
                  {(result.name || '?')[0].toUpperCase()}
                </div>
                <div>
                  <div className="font-semibold text-lg">{result.name}</div>
                  <div className="text-sm text-muted-foreground">
                    Confidence: {Math.round((result.confidence || 0) * 100)}%
                  </div>
                </div>
              </div>
              {/* Confidence bar */}
              <div className="mt-3 h-2 rounded-full bg-muted overflow-hidden">
                <div
                  className="h-full rounded-full bg-green-500 transition-all"
                  style={{ width: `${Math.round((result.confidence || 0) * 100)}%` }}
                />
              </div>
              <div className="text-xs text-muted-foreground mt-1">
                {result.process_time_ms}ms
              </div>
            </>
          ) : (
            <div className="flex items-center gap-3">
              <div className="w-10 h-10 rounded-full bg-muted flex items-center justify-center text-muted-foreground text-xl">?</div>
              <div>
                <div className="font-semibold">Unknown</div>
                <div className="text-sm text-muted-foreground">
                  {result.error || 'No match found in the face library.'}
                </div>
              </div>
            </div>
          )}
        </div>
      )}
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Unknown People from camera events
// ─────────────────────────────────────────────────────────────────────────────

function UnknownPeoplePanel({ onTrained }) {
  const [items, setItems] = useState([]);
  const [names, setNames] = useState({});
  const [busyId, setBusyId] = useState(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState(null);

  const loadUnknownEvents = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const cropData = await apiFetch(`${FACE_API_BASE}/unknown-crops?limit=48`);
      const crops = Array.isArray(cropData.crops)
        ? cropData.crops.map(crop => ({
            kind: 'crop',
            id: crop.id,
            event_id: crop.event_id,
            stream_name: crop.stream_name,
            confidence: crop.confidence,
            image_url: crop.crop_url,
            updated_at: crop.updated_at,
          }))
        : [];

      if (crops.length > 0) {
        setItems(crops);
        return;
      }

      const data = await apiFetch('/api/events?label=person&limit=48');
      const fallbackEvents = Array.isArray(data.events)
        ? data.events
            .filter(event =>
              event.snapshot_url &&
              (!event.sub_label || event.sub_label === 'Unknown')
            )
            .map(event => ({
              kind: 'event',
              id: event.id,
              event_id: event.id,
              stream_name: event.stream_name,
              confidence: event.best_confidence,
              image_url: event.snapshot_url,
              updated_at: event.updated_at,
            }))
        : [];
      setItems(fallbackEvents);
    } catch (err) {
      setError(err.message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    loadUnknownEvents();
  }, [loadUnknownEvents]);

  const handleTrain = async (item) => {
    const key = `${item.kind}-${item.id}`;
    const name = (names[key] || '').trim();
    if (!name) return;

    setBusyId(key);
    setError(null);
    try {
      const endpoint = item.kind === 'crop'
        ? `${FACE_API_BASE}/train-crop`
        : `${FACE_API_BASE}/train-event`;
      const body = item.kind === 'crop'
        ? { crop_id: item.id, name }
        : { event_id: item.event_id, name };

      const data = await apiFetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      });
      if (!data.success) {
        throw new Error(data.upstream_response || 'Training failed');
      }
      setNames(prev => ({ ...prev, [key]: '' }));
      setItems(prev => prev.filter(candidate => `${candidate.kind}-${candidate.id}` !== key));
      onTrained?.();
    } catch (err) {
      setError(err.message);
    } finally {
      setBusyId(null);
    }
  };

  return (
    <div className="bg-card rounded-2xl border border-border shadow-sm p-5">
      <div className="flex items-center justify-between gap-3 mb-4">
        <div>
          <h3 className="text-base font-semibold">Identify Unrecognized People</h3>
          <p className="text-xs text-muted-foreground mt-1">
            Name detected face crops from camera events instead of uploading photos.
          </p>
        </div>
        <button
          type="button"
          className="btn-secondary text-sm"
          onClick={loadUnknownEvents}
          disabled={loading}
        >
          {loading ? 'Loading...' : 'Refresh'}
        </button>
      </div>

      {error && (
        <div className="rounded-lg border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive mb-4">
          {error}
        </div>
      )}

      {loading ? (
        <div className="grid grid-cols-2 md:grid-cols-4 gap-3">
          {Array.from({ length: 4 }).map((_, i) => (
            <div key={i} className="aspect-[4/3] rounded-xl bg-muted animate-pulse" />
          ))}
        </div>
      ) : items.length === 0 ? (
        <div className="rounded-xl border border-dashed border-border bg-muted/30 p-6 text-center text-sm text-muted-foreground">
          No unrecognized person snapshots yet.
        </div>
      ) : (
        <div className="grid grid-cols-1 sm:grid-cols-2 xl:grid-cols-3 gap-4">
          {items.map(item => {
            const key = `${item.kind}-${item.id}`;
            return (
            <div key={key} className="rounded-xl border border-border overflow-hidden bg-background">
              <img
                src={`${item.image_url}?v=${item.updated_at || item.id}`}
                alt={`Unknown face from event ${item.event_id}`}
                className={`${item.kind === 'crop' ? 'aspect-square' : 'aspect-video'} w-full object-cover bg-muted`}
              />
              <div className="p-3 space-y-2">
                <div className="text-xs text-muted-foreground">
                  {item.stream_name} · {Math.round((item.confidence || 0) * 100)}% · {item.kind === 'crop' ? 'face crop' : 'event snapshot'}
                </div>
                <div className="flex gap-2">
                  <input
                    type="text"
                    className="input input-bordered flex-1 min-w-0"
                    placeholder="Name"
                    value={names[key] || ''}
                    onInput={(e) => setNames(prev => ({ ...prev, [key]: e.target.value }))}
                    maxLength={64}
                  />
                  <button
                    type="button"
                    className="btn-primary px-3 disabled:opacity-50"
                    disabled={busyId === key || !(names[key] || '').trim()}
                    onClick={() => handleTrain(item)}
                  >
                    {busyId === key ? 'Saving...' : 'Name'}
                  </button>
                </div>
                <a
                  className="text-xs text-primary hover:underline"
                  href={`events.html?event=${item.event_id}`}
                >
                  Open event
                </a>
              </div>
            </div>
          )})}
        </div>
      )}
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Face Card
// ─────────────────────────────────────────────────────────────────────────────

function FaceCard({ face, onDelete }) {
  const [confirmDelete, setConfirmDelete] = useState(false);
  const [deleting, setDeleting] = useState(false);

  const handleDelete = async () => {
    setDeleting(true);
    try {
      await apiFetch(`${FACE_API_BASE}/${face.id}`, { method: 'DELETE' });
      onDelete?.(face.id);
    } catch (err) {
      console.error('Delete face error:', err);
      setDeleting(false);
      setConfirmDelete(false);
    }
  };

  // Use initials as placeholder avatar
  const initials = (face.name || '?').split(' ').map(w => w[0]).join('').toUpperCase().slice(0, 2);
  const trainedDate = formatFaceDate(face.last_trained_at || face.first_trained_at);

  return (
    <div className="group relative bg-card rounded-2xl border border-border shadow-sm overflow-hidden flex flex-col transition-shadow hover:shadow-md">
      {/* Avatar area */}
      <div className="aspect-square bg-gradient-to-br from-primary/20 to-primary/5 flex items-center justify-center text-5xl font-bold text-primary/80 select-none">
        {face.thumbnail_url ? (
          <img
            src={face.thumbnail_url}
            alt={face.name}
            className="w-full h-full object-cover"
          />
        ) : (
          <span>{initials}</span>
        )}
      </div>

      {/* Info area */}
      <div className="p-3 flex flex-col gap-1 flex-1">
        <div className="font-semibold truncate">{face.name}</div>
        <div className="text-xs text-muted-foreground leading-snug">
          <div>
            {face.sample_count != null ? `${face.sample_count} sample${face.sample_count !== 1 ? 's' : ''}` : 'Trained'}
          </div>
          {trainedDate && <div>Latest: {trainedDate}</div>}
        </div>
      </div>

      {/* Delete button */}
      {confirmDelete ? (
        <div className="absolute inset-0 bg-background/90 backdrop-blur flex flex-col items-center justify-center gap-3 p-4">
          <p className="text-sm font-medium text-center">Remove <strong>{face.name}</strong> from the library?</p>
          <div className="flex gap-2">
            <button
              onClick={handleDelete}
              disabled={deleting}
              className="btn-primary bg-destructive hover:bg-destructive/90 text-destructive-foreground text-sm px-4 py-1.5 disabled:opacity-50"
            >
              {deleting ? 'Removing…' : 'Remove'}
            </button>
            <button
              onClick={() => setConfirmDelete(false)}
              className="btn-secondary text-sm px-4 py-1.5"
            >
              Cancel
            </button>
          </div>
        </div>
      ) : (
        <button
          onClick={() => setConfirmDelete(true)}
          className="absolute top-2 right-2 p-1.5 rounded-full bg-background/70 backdrop-blur border border-border opacity-0 group-hover:opacity-100 transition-opacity hover:bg-destructive hover:text-destructive-foreground hover:border-destructive"
          title="Delete face"
        >
          <svg className="w-4 h-4" fill="none" viewBox="0 0 24 24" stroke="currentColor">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2}
              d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6m1-10V4a1 1 0 00-1-1h-4a1 1 0 00-1 1v3M4 7h16" />
          </svg>
        </button>
      )}
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings tip banner
// ─────────────────────────────────────────────────────────────────────────────

function SettingsTip({ enabled, apiUrl }) {
  if (enabled && apiUrl) return null;
  return (
    <div className="flex items-start gap-3 rounded-xl border border-yellow-400/50 bg-yellow-400/10 p-4 text-sm text-yellow-700 dark:text-yellow-300">
      <svg className="w-5 h-5 mt-0.5 shrink-0" fill="none" viewBox="0 0 24 24" stroke="currentColor">
        <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2}
          d="M12 9v2m0 4h.01M10.29 3.86L1.82 18a2 2 0 001.71 3h16.94a2 2 0 001.71-3L13.71 3.86a2 2 0 00-3.42 0z" />
      </svg>
      <div>
        <strong>Face Recognition is not fully configured.</strong><br />
        Go to <a href="settings.html#detection" className="underline hover:no-underline">Settings → Detection</a> and enable{' '}
        <em>Face Recognition</em>, then set the API URL to{' '}
        <code className="bg-black/10 dark:bg-white/10 rounded px-1">
          http://light-object-detect:8000/api/v1/faces/recognize
        </code>
        .
      </div>
    </div>
  );
}

// ─────────────────────────────────────────────────────────────────────────────
// Main FacesView
// ─────────────────────────────────────────────────────────────────────────────

export function FacesView() {
  const [faces, setFaces] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState(null);
  const [settings, setSettings] = useState({ enabled: false, apiUrl: '' });

  const loadFaces = useCallback(async () => {
    setLoading(true);
    setError(null);
    try {
      const data = await apiFetch(`${FACE_API_BASE}/list`);
      setFaces(Array.isArray(data.faces) ? data.faces : []);
    } catch (err) {
      setError(err.message);
    } finally {
      setLoading(false);
    }
  }, []);

  const loadSettings = useCallback(async () => {
    try {
      const data = await apiFetch('/api/settings');
      setSettings({
        enabled: Boolean(data.face_recognition_enabled),
        apiUrl: data.face_recognition_api_url || '',
      });
    } catch {
      // ignore settings fetch error
    }
  }, []);

  useEffect(() => {
    loadFaces();
    loadSettings();
  }, [loadFaces, loadSettings]);

  const handleDelete = useCallback((deletedId) => {
    setFaces((prev) => prev.filter((f) => f.id !== deletedId));
  }, []);

  return (
    <div className="space-y-6 pb-10">
      {/* Page header */}
      <div className="flex flex-col gap-1 sm:flex-row sm:items-end sm:justify-between">
        <div>
          <h2 className="text-2xl font-semibold m-0 flex items-center gap-2">
            <svg className="w-7 h-7 text-primary" fill="none" viewBox="0 0 24 24" stroke="currentColor">
              <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.8}
                d="M17 20h5v-2a3 3 0 00-5.356-1.857M17 20H7m10 0v-2c0-.656-.126-1.283-.356-1.857M7 20H2v-2a3 3 0 015.356-1.857M7 20v-2c0-.656.126-1.283.356-1.857m0 0a5.002 5.002 0 019.288 0M15 7a3 3 0 11-6 0 3 3 0 016 0z" />
            </svg>
            Face Library
          </h2>
          <p className="text-sm text-muted-foreground mt-1">
            Train known faces so LightNVR can automatically name people in events.
          </p>
        </div>
        <button
          onClick={loadFaces}
          disabled={loading}
          className="btn-secondary self-start sm:self-auto"
        >
          {loading ? 'Loading…' : 'Refresh'}
        </button>
      </div>

      <SettingsTip enabled={settings.enabled} apiUrl={settings.apiUrl} />

      <UnknownPeoplePanel onTrained={loadFaces} />

      {/* Two-column layout on large screens */}
      <div className="grid grid-cols-1 lg:grid-cols-[340px_1fr] gap-6">
        {/* Left: Train + Test panels */}
        <div className="flex flex-col gap-4">
          <TrainFacePanel onTrained={loadFaces} />
          <TestRecognitionPanel />
        </div>

        {/* Right: Face library grid */}
        <div>
          {error && (
            <div className="rounded-xl border border-destructive/40 bg-destructive/10 p-4 text-sm text-destructive mb-4">
              Failed to load face library: {error}
            </div>
          )}

          {loading ? (
            <div className="grid grid-cols-2 sm:grid-cols-3 xl:grid-cols-4 gap-4">
              {Array.from({ length: 6 }).map((_, i) => (
                <div key={i} className="aspect-square rounded-2xl bg-muted animate-pulse" />
              ))}
            </div>
          ) : faces.length === 0 ? (
            <div className="rounded-xl border border-dashed border-border bg-card/50 flex flex-col items-center justify-center py-16 gap-3 text-center">
              <svg className="w-12 h-12 text-muted-foreground/50" fill="none" viewBox="0 0 24 24" stroke="currentColor">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1.5}
                  d="M17 20h5v-2a3 3 0 00-5.356-1.857M17 20H7m10 0v-2c0-.656-.126-1.283-.356-1.857M7 20H2v-2a3 3 0 015.356-1.857M7 20v-2c0-.656.126-1.283.356-1.857m0 0a5.002 5.002 0 019.288 0M15 7a3 3 0 11-6 0 3 3 0 016 0z" />
              </svg>
              <div>
                <p className="font-medium">No faces trained yet</p>
                <p className="text-sm text-muted-foreground mt-1">
                  Use the panel on the left to add your first person.
                </p>
              </div>
            </div>
          ) : (
            <>
              <p className="text-sm text-muted-foreground mb-3">
                {faces.length} person{faces.length !== 1 ? 's' : ''} in library
              </p>
              <div className="grid grid-cols-2 sm:grid-cols-3 xl:grid-cols-4 gap-4">
                {faces.map((face) => (
                  <FaceCard key={face.id} face={face} onDelete={handleDelete} />
                ))}
              </div>
            </>
          )}
        </div>
      </div>
    </div>
  );
}

export default FacesView;
