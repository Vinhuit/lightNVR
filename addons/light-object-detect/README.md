# Light Object Detect

Separate Home Assistant app providing the detection API used by LightNVR.
CPU inference with ONNX; initially supports amd64 only, matching the pinned upstream image.

Install this app and LightNVR from the same repository. Start detection first,
then set LightNVR's `detection_server` to `http://<detection-app-hostname>:8000`.
See DOCS.md for the hostname and persistent storage details.
