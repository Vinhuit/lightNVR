# Installation

## Local installation before publishing

1. Copy **this entire `addons/lightnvr` directory** to `/addons/lightnvr` on
   your Home Assistant OS/Supervisor host. The resulting file is
   `/addons/lightnvr/config.yaml`.
2. Open **Settings → Add-ons → Add-on Store**, then use **Check for updates**
   or reload the store. LightNVR should appear under **Local add-ons**.
3. Install LightNVR. Supervisor builds the small wrapper image locally.
4. Optionally set `initial_admin_password` before the first start; otherwise
   a random password is printed once in the add-on log.
5. Start the add-on, enable **Show in sidebar**, and open its web interface.
6. Log in as `admin`, then add cameras in **Streams**. A LightNVR login is
   required inside Ingress so the NVR retains its own user permissions.

## Repository installation after publishing

Once `repository.yaml` and `addons/lightnvr/` have been committed and pushed
to the repository, add `https://github.com/Vinhuit/lightNVR` under the
Add-on Store's **Repositories** menu. Then install **LightNVR** from that
repository. Local changes alone do not make the GitHub installation available.

# Options

```yaml
retention_days: 30
log_level: info
initial_admin_password: ""
# Optional:
# detection_server: http://your-detector:8000/detect
# onvif_network: 192.168.1.0/24
# webrtc_external_ip: 192.168.1.10
```

| Option | Meaning |
| --- | --- |
| `retention_days` | Recording retention, 1–3650 days. Applied at every start. |
| `log_level` | `error`, `warning`, `info`, or `debug`. Applied at every start. |
| `initial_admin_password` | Used only when creating a new configuration. Empty generates a random password. A supplied password must contain 12–63 characters with no whitespace, `;`, or `#`. Change existing users' passwords in LightNVR Users. |
| `detection_server` | Optional external HTTP(S) detection endpoint. The AI service is installed separately; it is not included in this add-on. |
| `onvif_network` | Optional IPv4 camera network in CIDR notation. Enables discovery; adding cameras by RTSP URL works without it. Bridge networking may limit multicast discovery. |
| `webrtc_external_ip` | Optional reachable host IP advertised to WebRTC clients. Use your Home Assistant LAN IP if video cannot connect. |

Optional detection, ONVIF and external-IP values update the corresponding
LightNVR settings when supplied. Omitting them preserves the existing NVR
configuration; clear them in LightNVR Settings when no longer needed.
Home Assistant provides the container timezone through `TZ`.

# Network

Ingress uses an internal proxy on port 8099; it accepts connections only
from Supervisor (`172.30.32.2`). It is not published to the host. The default
installation exposes only WebRTC media on TCP/UDP 8555. If another service
already uses that port, resolve the conflict before starting.

Under the add-on's **Network** settings, you can optionally enable:

- `8080/tcp`: direct LightNVR web interface. LightNVR authentication remains enabled.
- `8554/tcp`: RTSP restream, for example `rtsp://HA_IP:8554/camera-name`.

Keep the WebRTC host mapping at 8555, matching the configured ICE port.
Ingress transports the signaling and WebSocket requests; the camera media
still needs a reachable TCP/UDP media port. HLS is available when WebRTC is
unsuitable. The go2rtc API is intentionally kept inside the container.

# Persistence and backup

| Data | Container path |
| --- | --- |
| Editable INI configuration and go2rtc overrides | `/etc/lightnvr` (Supervisor add-on configuration volume) |
| Database and migrations state | `/data/database` |
| Models | `/data/models` |
| HLS cache | `/data/hls` |
| Application logs | `/data/log` |
| Recordings | `/media/lightnvr/recordings` |

Do not mount over `/var/lib/lightnvr/www`; it contains the built frontend.
Startup preserves custom stream definitions and other user settings, while
enforcing the storage paths and ports that the add-on needs.

The add-on uses a **cold backup** so Supervisor stops LightNVR before
backing up its database. Recordings are in Home Assistant's shared Media
directory; include **Media** in a full Home Assistant backup, or back up
recordings separately. An add-on-only backup does not cover shared Media.

# Development

The Dockerfile pins the public enriched image by digest for reproducible
runtime selection. Its frontend already supports Home Assistant's Ingress
base path. This image is amd64 only; do not add ARM to `arch` before an ARM
image with the same functionality is published and tested.

```sh
python -m unittest discover -s tests -p test_hass_addon.py
docker build -t lightnvr-hass-addon:0.1.0 addons/lightnvr
```

The root `repository.yaml` makes this source repository discoverable by
Supervisor. No `image` field is set in `config.yaml`, so Supervisor builds
the wrapper from this directory without requiring an add-on image release.

The Docker build argument `LIGHTNVR_IMAGE` may be overridden for development;
the replacement must provide `/bin/lightnvr`, `/bin/go2rtc`, the
`--generate-go2rtc-config` command, and frontend assets with Ingress support.

# References and license

- [Frigate Home Assistant add-ons](https://github.com/blakeblackshear/frigate-hass-addons)
- [Home Assistant add-on configuration](https://developers.home-assistant.io/docs/add-ons/configuration/)
- [Home Assistant Ingress requirements](https://developers.home-assistant.io/docs/add-ons/presentation/#ingress)

This wrapper follows the repository's GPL-3.0 license. It does not install
ytstream or an external AI detection server; those are separate services.
# Companion detection app

Install **Light Object Detect** from this same app repository. Start it and copy
its hostname from the app information page. Set this app's `detection_server`
option to `http://HOSTNAME:8000` and restart LightNVR. The detector runs in its own
container and keeps face/classification data in its own persistent storage.
No host port mapping is needed for communication between Supervisor apps.
