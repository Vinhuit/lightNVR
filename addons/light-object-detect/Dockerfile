ARG DETECTION_IMAGE=caubequay00/light-object-detect@sha256:46faeb17dd7d1c37dc07b27e6810e05301407176f2fbcf193ebc647ba5f0eeb3
FROM ${DETECTION_IMAGE}
ARG BUILD_VERSION=0.1.0
ARG BUILD_ARCH=amd64
LABEL io.hass.version="${BUILD_VERSION}" \
      io.hass.type="app" \
      io.hass.arch="${BUILD_ARCH}" \
      org.opencontainers.image.title="Light Object Detect Home Assistant App"
COPY run.py /usr/local/bin/light-object-detect-addon.py
WORKDIR /app
EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=5s --start-period=180s --retries=3 CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/health', timeout=4)" || exit 1
ENTRYPOINT ["python", "/usr/local/bin/light-object-detect-addon.py"]
CMD []
