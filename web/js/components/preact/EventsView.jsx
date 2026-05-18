/**
 * LightNVR Web Interface EventsView Component
 */

import { Fragment } from 'preact';
import { useMemo, useState } from 'preact/hooks';
import { useQuery } from '../../query-client.js';
import { useI18n } from '../../i18n.js';
import { formatUtils } from './recordings/formatUtils.js';

function formatEventTime(value) {
  if (!value) return '-';
  return new Date(value * 1000).toLocaleString();
}

function formatConfidence(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric) || numeric <= 0) return '-';
  return `${Math.round(numeric * 100)}%`;
}

function getBadgeClass(status) {
  switch ((status || '').toLowerCase()) {
    case 'ended':
    case 'complete':
    case 'completed':
      return 'badge-success';
    case 'active':
    case 'queued':
    case 'running':
      return 'badge-info';
    case 'failed':
    case 'error':
      return 'badge-danger';
    default:
      return 'badge-muted';
  }
}

function normalizeEvents(response) {
  if (Array.isArray(response)) return response;
  return Array.isArray(response?.events) ? response.events : [];
}

function normalizeStreams(response) {
  const streams = Array.isArray(response)
    ? response
    : Array.isArray(response?.streams)
      ? response.streams
      : [];
  return streams
    .map((stream) => (typeof stream === 'string' ? stream : stream?.name || stream?.id || ''))
    .filter(Boolean)
    .sort((a, b) => a.localeCompare(b));
}

function normalizeLabels(response) {
  const labels = Array.isArray(response?.labels) ? response.labels : [];
  return labels
    .map((item) => (typeof item === 'string' ? item : item?.label))
    .filter(Boolean)
    .sort((a, b) => a.localeCompare(b));
}

function hasEventSnapshot(event) {
  return Boolean(event?.snapshot_url || event?.thumbnail_path);
}

function buildEventSnapshotUrl(event) {
  const baseUrl = event.snapshot_url || `/api/events/${event.id}/snapshot`;
  const version = event.updated_at || event.best_time || event.start_time || '';
  return `${baseUrl}?v=${encodeURIComponent(version)}`;
}

function buildEventTimelineUrl(event) {
  return formatUtils.getTimelineUrl(event.stream_name, event.best_time || event.start_time);
}

function clampPercent(value) {
  const numeric = Number(value);
  if (!Number.isFinite(numeric)) return 0;
  return Math.max(0, Math.min(100, numeric * 100));
}

function getEventBox(event) {
  const box = event?.best_box || event?.bbox || event?.box;
  if (!box) return null;

  const x = Number(box.x);
  const y = Number(box.y);
  const width = Number(box.width ?? box.w);
  const height = Number(box.height ?? box.h);
  if (![x, y, width, height].every(Number.isFinite) || width <= 0 || height <= 0) {
    return null;
  }

  return {
    x: clampPercent(x),
    y: clampPercent(y),
    width: Math.max(1, Math.min(100, width * 100)),
    height: Math.max(1, Math.min(100, height * 100)),
    label: box.label || event?.label || '',
    confidence: Number.isFinite(Number(box.confidence)) ? Number(box.confidence) : event?.best_confidence
  };
}

function EventBoxOverlay({ event, compact = false }) {
  const box = getEventBox(event);
  if (!box) return null;

  const label = box.label
    ? `${box.label}${formatConfidence(box.confidence) !== '-' ? ` ${formatConfidence(box.confidence)}` : ''}`
    : '';

  return (
    <div
      className="pointer-events-none absolute border-2 border-emerald-400 shadow-[0_0_0_1px_rgba(0,0,0,0.65)]"
      style={{
        left: `${box.x}%`,
        top: `${box.y}%`,
        width: `${Math.min(box.width, 100 - box.x)}%`,
        height: `${Math.min(box.height, 100 - box.y)}%`
      }}
    >
      {!compact && label && (
        <div className="absolute left-0 top-0 max-w-full truncate rounded-br bg-emerald-500 px-1.5 py-0.5 text-xs font-semibold text-white shadow">
          {label}
        </div>
      )}
    </div>
  );
}

function EventSnapshot({ event, variant = 'thumb', onOpen }) {
  if (!hasEventSnapshot(event)) {
    const placeholderClass = variant === 'thumb'
      ? 'h-14 w-24 text-xs'
      : 'w-full aspect-video text-sm';
    return (
      <div className={`${placeholderClass} rounded bg-muted flex items-center justify-center text-muted-foreground`}>
        No image
      </div>
    );
  }

  const isModal = variant === 'modal';
  const wrapperClass = variant === 'thumb'
    ? 'relative h-14 w-24 overflow-hidden rounded bg-muted'
    : isModal
      ? 'relative inline-block max-w-full'
      : 'relative w-full aspect-video overflow-hidden rounded bg-muted';
  const imgClass = variant === 'thumb'
    ? 'h-full w-full object-cover'
    : isModal
      ? 'block max-h-[78vh] max-w-[90vw] rounded bg-muted'
      : 'h-full w-full object-cover';

  const image = (
    <div className={wrapperClass}>
      <img
        src={buildEventSnapshotUrl(event)}
        alt={`${event.label || 'event'} snapshot`}
        className={imgClass}
        loading="lazy"
      />
      <EventBoxOverlay event={event} compact={variant === 'thumb'} />
    </div>
  );

  if (!onOpen) return image;

  return (
    <button
      type="button"
      className="block rounded focus:outline-none focus:ring-2 focus:ring-primary"
      onClick={() => onOpen(event)}
      title="Open snapshot"
    >
      {image}
    </button>
  );
}

function SnapshotModal({ event, onClose }) {
  if (!event) return null;

  return (
    <div
      className="fixed inset-0 z-50 flex items-center justify-center bg-black/75 p-4"
      onClick={onClose}
      role="dialog"
      aria-modal="true"
      aria-label="Event snapshot"
    >
      <div className="max-w-[94vw]" onClick={(e) => e.stopPropagation()}>
        <div className="mb-3 flex items-center justify-between gap-3 text-white">
          <div className="min-w-0">
            <div className="truncate text-sm font-semibold">
              {event.stream_name || 'Event'} - {event.label || 'object'} - {formatEventTime(event.best_time || event.start_time)}
            </div>
          </div>
          <button type="button" className="btn-secondary bg-white text-black hover:bg-white/90" onClick={onClose}>
            Close
          </button>
        </div>
        <EventSnapshot event={event} variant="modal" />
      </div>
    </div>
  );
}

function buildQueryString(filters) {
  const params = new URLSearchParams();
  params.set('limit', '100');
  if (filters.stream.trim()) params.set('stream', filters.stream.trim());
  if (filters.label.trim()) params.set('label', filters.label.trim());
  if (filters.status !== 'all') params.set('status', filters.status);
  return params.toString();
}

function getInitialFocusedEventId() {
  if (typeof window === 'undefined') return null;
  const raw = new URLSearchParams(window.location.search).get('event');
  const numeric = Number(raw);
  return Number.isInteger(numeric) && numeric > 0 ? numeric : null;
}

function EventEnrichments({ eventId }) {
  const { t } = useI18n();
  const { data, isLoading, error } = useQuery(
    ['event-enrichments', eventId],
    `/api/events/${eventId}/enrichments`,
    { timeout: 10000, retries: 1 },
    { enabled: Boolean(eventId) }
  );

  const enrichments = Array.isArray(data?.enrichments) ? data.enrichments : [];

  if (isLoading) {
    return <div className="text-sm text-muted-foreground py-3">{t('common.loading')}</div>;
  }

  if (error) {
    return <div className="text-sm text-destructive py-3">Failed to load enrichments: {error.message}</div>;
  }

  if (enrichments.length === 0) {
    return <div className="text-sm text-muted-foreground py-3">No enrichment results for this event yet.</div>;
  }

  return (
    <div className="overflow-x-auto py-3">
      <table className="min-w-full divide-y divide-border text-sm">
        <thead>
          <tr>
            <th className="px-3 py-2 text-left">Type</th>
            <th className="px-3 py-2 text-left">Provider</th>
            <th className="px-3 py-2 text-left">Status</th>
            <th className="px-3 py-2 text-left">Value</th>
            <th className="px-3 py-2 text-left">Score</th>
          </tr>
        </thead>
        <tbody className="divide-y divide-border">
          {enrichments.map((item) => (
            <tr key={item.id}>
              <td className="px-3 py-2">{item.type || '-'}</td>
              <td className="px-3 py-2">{item.provider || '-'}</td>
              <td className="px-3 py-2">
                <span className={getBadgeClass(item.status)}>{item.status || 'unknown'}</span>
              </td>
              <td className="px-3 py-2 max-w-md truncate" title={item.value || item.error || ''}>
                {item.value || item.error || '-'}
              </td>
              <td className="px-3 py-2">{formatConfidence(item.score)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export function EventsView() {
  const { t } = useI18n();
  const [focusedEventId, setFocusedEventId] = useState(getInitialFocusedEventId);
  const [filters, setFilters] = useState({ stream: '', label: '', status: 'all' });
  const [appliedFilters, setAppliedFilters] = useState(filters);
  const [expandedEventId, setExpandedEventId] = useState(() => getInitialFocusedEventId());
  const [snapshotModalEvent, setSnapshotModalEvent] = useState(null);

  const { data: streamsData } = useQuery(
    ['events-streams'],
    '/api/streams',
    { timeout: 10000, retries: 1 }
  );

  const { data: labelsData } = useQuery(
    ['events-detection-labels'],
    '/api/recordings/detection-labels',
    { timeout: 10000, retries: 1 }
  );

  const queryString = useMemo(() => buildQueryString(appliedFilters), [appliedFilters]);
  const {
    data,
    isLoading,
    error,
    refetch,
    isFetching
  } = useQuery(
    ['events', queryString],
    `/api/events?${queryString}`,
    { timeout: 10000, retries: 1 },
    { enabled: !focusedEventId }
  );

  const {
    data: focusedEventData,
    isLoading: focusedEventLoading,
    error: focusedEventError,
    refetch: refetchFocusedEvent,
    isFetching: focusedEventFetching
  } = useQuery(
    ['event', focusedEventId],
    focusedEventId ? `/api/events/${focusedEventId}` : null,
    { timeout: 10000, retries: 1 },
    { enabled: Boolean(focusedEventId) }
  );

  const listedEvents = normalizeEvents(data);
  const focusedEvent = focusedEventData && !Array.isArray(focusedEventData?.events)
    ? focusedEventData
    : null;
  const events = focusedEvent ? [focusedEvent] : listedEvents;
  const currentLoading = focusedEventId ? focusedEventLoading : isLoading;
  const currentError = focusedEventId ? focusedEventError : error;
  const currentFetching = focusedEventId ? focusedEventFetching : isFetching;

  const streamOptions = useMemo(() => {
    const fromApi = normalizeStreams(streamsData);
    const fromEvents = [...listedEvents, ...events].map((event) => event.stream_name).filter(Boolean);
    return Array.from(new Set([...fromApi, ...fromEvents]))
      .sort((a, b) => a.localeCompare(b));
  }, [streamsData, listedEvents, events]);
  const labelOptions = useMemo(() => {
    const fromApi = normalizeLabels(labelsData);
    const fromEvents = [...listedEvents, ...events].map((event) => event.label).filter(Boolean);
    return Array.from(new Set([...fromApi, ...fromEvents]))
      .sort((a, b) => a.localeCompare(b));
  }, [labelsData, listedEvents, events]);

  const handleFilterChange = (e) => {
    const { name, value } = e.target;
    setFilters((current) => ({ ...current, [name]: value }));
  };

  const applyFilters = (e) => {
    e.preventDefault();
    setAppliedFilters(filters);
  };

  const resetFilters = () => {
    const nextFilters = { stream: '', label: '', status: 'all' };
    setFilters(nextFilters);
    setAppliedFilters(nextFilters);
  };

  const showAllEvents = () => {
    if (typeof window !== 'undefined') {
      const url = new URL(window.location.href);
      url.searchParams.delete('event');
      window.history.replaceState({}, '', url.toString());
    }
    setFocusedEventId(null);
    setExpandedEventId(null);
  };

  return (
    <div className="space-y-4">
      <div className="flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
        <div>
          <h2 className="text-2xl font-semibold m-0">Events</h2>
          <p className="text-sm text-muted-foreground mt-1 mb-0">
            Grouped detections, face labels, license text, and GenAI enrichment results.
          </p>
        </div>
        <button
          type="button"
          className="btn-secondary self-start md:self-auto"
          onClick={() => focusedEventId ? refetchFocusedEvent() : refetch()}
          disabled={currentFetching}
        >
          {currentFetching ? t('common.loading') : t('common.refresh')}
        </button>
      </div>

      {focusedEventId && (
        <div className="flex flex-col gap-3 rounded-lg border border-primary/30 bg-primary/10 p-4 sm:flex-row sm:items-center sm:justify-between">
          <div className="text-sm">
            <div className="font-medium">Showing event #{focusedEventId}</div>
            <div className="text-muted-foreground">Opened from the face review panel.</div>
          </div>
          <button type="button" className="btn-secondary self-start sm:self-auto" onClick={showAllEvents}>
            Show all events
          </button>
        </div>
      )}

      {!focusedEventId && (
        <form
          className="grid grid-cols-1 md:grid-cols-[1fr_1fr_auto_auto] gap-3 items-end rounded-lg border border-border bg-card p-4"
          onSubmit={applyFilters}
        >
          <label className="flex flex-col gap-1">
            <span className="text-sm font-medium">Stream</span>
            <input
              name="stream"
              list="events-stream-options"
              className="input input-bordered w-full"
              value={filters.stream}
              onInput={handleFilterChange}
              placeholder="All streams"
              autoComplete="off"
            />
            <datalist id="events-stream-options">
              {streamOptions.map((stream) => (
                <option key={stream} value={stream}>{stream}</option>
              ))}
            </datalist>
          </label>
          <label className="flex flex-col gap-1">
            <span className="text-sm font-medium">Object</span>
            <input
              name="label"
              list="events-object-options"
              className="input input-bordered w-full"
              value={filters.label}
              onInput={handleFilterChange}
              placeholder="All objects"
              autoComplete="off"
            />
            <datalist id="events-object-options">
              {labelOptions.map((label) => (
                <option key={label} value={label}>{label}</option>
              ))}
            </datalist>
          </label>
          <label className="flex flex-col gap-1">
            <span className="text-sm font-medium">Status</span>
            <select
              name="status"
              className="select select-bordered w-full min-w-32"
              value={filters.status}
              onChange={handleFilterChange}
            >
              <option value="all">All</option>
              <option value="active">Active</option>
              <option value="ended">Ended</option>
            </select>
          </label>
          <div className="flex gap-2">
            <button type="submit" className="btn-primary">Apply</button>
            <button type="button" className="btn-secondary" onClick={resetFilters}>Reset</button>
          </div>
        </form>
      )}

      {currentError && (
        <div className="rounded-lg border border-destructive/40 bg-destructive/10 p-4 text-sm text-destructive">
          Failed to load {focusedEventId ? `event #${focusedEventId}` : 'events'}: {currentError.message}
        </div>
      )}

      {currentLoading ? (
        <div className="text-center py-8 text-muted-foreground">{t('common.loadingData')}</div>
      ) : events.length === 0 ? (
        <div className="rounded-lg border border-border bg-card p-6 text-center text-muted-foreground">
          No events yet. Enable object detection on a stream and wait for detections to create grouped events.
        </div>
      ) : (
        <div className="overflow-x-auto rounded-lg border border-border bg-card">
          <table className="min-w-full divide-y divide-border">
            <thead>
              <tr>
                <th className="px-4 py-3 text-left">Snapshot</th>
                <th className="px-4 py-3 text-left">Time</th>
                <th className="px-4 py-3 text-left">Stream</th>
                <th className="px-4 py-3 text-left">Object</th>
                <th className="px-4 py-3 text-left">Recognition</th>
                <th className="px-4 py-3 text-left">Confidence</th>
                <th className="px-4 py-3 text-left">Status</th>
                <th className="px-4 py-3 text-right">Actions</th>
              </tr>
            </thead>
            <tbody className="divide-y divide-border">
              {events.map((event) => (
                <Fragment key={event.id}>
                  <tr>
                    <td className="px-4 py-3">
                      <EventSnapshot event={event} variant="thumb" onOpen={setSnapshotModalEvent} />
                    </td>
                    <td className="px-4 py-3 whitespace-nowrap">{formatEventTime(event.best_time || event.start_time)}</td>
                    <td className="px-4 py-3">{event.stream_name || '-'}</td>
                    <td className="px-4 py-3">
                      <div className="font-medium">{event.label || '-'}</div>
                      {event.zone_id && <div className="text-xs text-muted-foreground">Zone {event.zone_id}</div>}
                    </td>
                    <td className="px-4 py-3">
                      {event.sub_label || event.recognized_text || <span className="text-muted-foreground">-</span>}
                    </td>
                    <td className="px-4 py-3">{formatConfidence(event.best_confidence)}</td>
                    <td className="px-4 py-3">
                      <span className={getBadgeClass(event.status)}>{event.status || 'unknown'}</span>
                    </td>
                    <td className="px-4 py-3">
                      <div className="flex flex-wrap justify-end gap-2">
                        <button
                          type="button"
                          className="btn-secondary"
                          onClick={() => setExpandedEventId(expandedEventId === event.id ? null : event.id)}
                        >
                          {expandedEventId === event.id ? 'Hide' : 'Details'}
                        </button>
                        <a
                          className="btn-secondary"
                          href={buildEventTimelineUrl(event)}
                          title="View in Timeline"
                        >
                          Timeline
                        </a>
                      </div>
                    </td>
                  </tr>
                  {expandedEventId === event.id && (
                    <tr>
                      <td className="px-4 py-3 bg-muted/30" colSpan="8">
                        <div className="grid grid-cols-1 lg:grid-cols-[220px_1fr] gap-4">
                          <div>
                            <EventSnapshot event={event} variant="detail" onOpen={setSnapshotModalEvent} />
                          </div>
                          <div>
                        <div className="grid grid-cols-1 md:grid-cols-4 gap-3 text-sm">
                          <div>
                            <div className="text-muted-foreground">Event ID</div>
                            <div>{event.id}</div>
                          </div>
                          <div>
                            <div className="text-muted-foreground">Start</div>
                            <div>{formatEventTime(event.start_time)}</div>
                          </div>
                          <div>
                            <div className="text-muted-foreground">End</div>
                            <div>{formatEventTime(event.end_time)}</div>
                          </div>
                          <div>
                            <div className="text-muted-foreground">Detection ID</div>
                            <div>{event.best_detection_id || '-'}</div>
                          </div>
                          </div>
                        <EventEnrichments eventId={event.id} />
                          </div>
                        </div>
                      </td>
                    </tr>
                  )}
                </Fragment>
              ))}
            </tbody>
          </table>
        </div>
      )}
      <SnapshotModal event={snapshotModalEvent} onClose={() => setSnapshotModalEvent(null)} />
    </div>
  );
}
