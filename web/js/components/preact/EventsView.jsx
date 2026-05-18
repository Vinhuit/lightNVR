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
  if (!Array.isArray(response)) return [];
  return response
    .map((stream) => stream?.name || stream?.id || '')
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

function buildQueryString(filters) {
  const params = new URLSearchParams();
  params.set('limit', '100');
  if (filters.stream.trim()) params.set('stream', filters.stream.trim());
  if (filters.label.trim()) params.set('label', filters.label.trim());
  if (filters.status !== 'all') params.set('status', filters.status);
  return params.toString();
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
  const [filters, setFilters] = useState({ stream: '', label: '', status: 'all' });
  const [appliedFilters, setAppliedFilters] = useState(filters);
  const [expandedEventId, setExpandedEventId] = useState(null);

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
    { timeout: 10000, retries: 1 }
  );

  const events = normalizeEvents(data);
  const streamOptions = useMemo(() => normalizeStreams(streamsData), [streamsData]);
  const labelOptions = useMemo(() => {
    const fromApi = normalizeLabels(labelsData);
    const fromEvents = events.map((event) => event.label).filter(Boolean);
    return Array.from(new Set([...fromApi, ...fromEvents]))
      .sort((a, b) => a.localeCompare(b));
  }, [labelsData, events]);

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
          onClick={() => refetch()}
          disabled={isFetching}
        >
          {isFetching ? t('common.loading') : t('common.refresh')}
        </button>
      </div>

      <form
        className="grid grid-cols-1 md:grid-cols-[1fr_1fr_auto_auto] gap-3 items-end rounded-lg border border-border bg-card p-4"
        onSubmit={applyFilters}
      >
        <label className="flex flex-col gap-1">
          <span className="text-sm font-medium">Stream</span>
          <select
            name="stream"
            className="select select-bordered w-full"
            value={filters.stream}
            onChange={handleFilterChange}
          >
            <option value="">All streams</option>
            {streamOptions.map((stream) => (
              <option key={stream} value={stream}>{stream}</option>
            ))}
          </select>
        </label>
        <label className="flex flex-col gap-1">
          <span className="text-sm font-medium">Object</span>
          <select
            name="label"
            className="select select-bordered w-full"
            value={filters.label}
            onChange={handleFilterChange}
          >
            <option value="">All objects</option>
            {labelOptions.map((label) => (
              <option key={label} value={label}>{label}</option>
            ))}
          </select>
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

      {error && (
        <div className="rounded-lg border border-destructive/40 bg-destructive/10 p-4 text-sm text-destructive">
          Failed to load events: {error.message}
        </div>
      )}

      {isLoading ? (
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
                      {hasEventSnapshot(event) ? (
                        <img
                          src={buildEventSnapshotUrl(event)}
                          alt={`${event.label || 'event'} snapshot`}
                          className="h-14 w-24 rounded object-cover bg-muted"
                          loading="lazy"
                        />
                      ) : (
                        <div className="h-14 w-24 rounded bg-muted flex items-center justify-center text-xs text-muted-foreground">
                          No image
                        </div>
                      )}
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
                            {hasEventSnapshot(event) ? (
                              <img
                                src={buildEventSnapshotUrl(event)}
                                alt={`${event.label || 'event'} snapshot`}
                                className="w-full aspect-video rounded object-cover bg-muted"
                                loading="lazy"
                              />
                            ) : (
                              <div className="w-full aspect-video rounded bg-muted flex items-center justify-center text-sm text-muted-foreground">
                                No image
                              </div>
                            )}
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
    </div>
  );
}
