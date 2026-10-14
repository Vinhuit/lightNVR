# Connecting LightNVR

Both apps run separate containers. Supervisor creates them; no Docker socket is needed.
Copy this app's hostname from its information page. For a local app it is normally
`local-light-object-detect`; a repository app has a repository hash prefix.
Set LightNVR's `detection_server` option to `http://HOSTNAME:8000` and restart LightNVR.
Use the base URL, without `/api` or `/v1`.

Port 8000 is available on the Supervisor internal network without a host port mapping.
Optional host exposure has no API authentication; use only on a trusted LAN.

Face data and classification data persist under `/data/detection`; downloaded model
caches persist under `/data/model-cache`. Supervisor backups include `/data`.
ONNX inference uses the model included in the pinned image. Optional GPU, Coral and
Moondream backends are not enabled by this initial CPU app.

Configure confidence threshold, inference threads, image size, face detection size
and classification in the app Options tab. More inference threads increase CPU usage.
