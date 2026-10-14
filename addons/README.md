# MixSmart camera apps

This repository contains two independent Home Assistant apps:
`lightnvr` and `light-object-detect`. Add `https://github.com/Vinhuit/lightNVR`
in the Home Assistant app store's repository settings. Supervisor discovers
both app directories here. Alternatively copy them into the local apps directory.

Start Light Object Detect, copy its internal hostname, and configure LightNVR's
`detection_server` as `http://HOSTNAME:8000`.

Home Assistant Container installations, such as the current machine at
192.168.1.198, do not include Supervisor or an app store. They must use Docker Compose
instead. The existing detection service on 192.168.1.204 remains available.
