# Fork Changelog (`ssysm/OvenMediaEngine`)

This file tracks all features, fixes, CI workflows, and modifications maintained in the `ssysm` fork on top of upstream [`OvenMediaLabs/OvenMediaEngine`](https://github.com/OvenMediaLabs/OvenMediaEngine).

---

## 2026-11-27

### Fixed
- **glibc < 2.37 & GCC 11 compatibility for `TCP_INFO` min RTT** (`6b2771b1`)
  - Mirrored the frozen Linux UAPI `tcp_info` struct prefix through `tcpi_min_rtt` in `src/base/ovsocket/socket.cpp` so builds on Ubuntu 22.04 / RHEL 9 (where `<netinet/tcp.h>` stops at `tcpi_total_retrans`) compile cleanly while still reading `tcpi_min_rtt` at runtime.
  - Added explicit standard header includes for GCC 11 compatibility (`openmetrics_renderer.cpp`, `src/base/provider/stream.h`, `src/config/config_manager.cpp`, `src/publishers/hls/hls_session.h`, `src/publishers/llhls/llhls_session.h`).

### Added
- **OpenMetrics v2 monitoring endpoints & telemetry** (`2f351212`)
  - Added `/v2/metrics` OpenMetrics 1.0.0 endpoints (`application/openmetrics-text; version=1.0.0; charset=utf-8`) with gzip compression (`Accept-Encoding`) and configurable response caching (`<Managers><API><OpenMetrics><Cache>`):
    - `GET /v2/metrics`
    - `GET /v2/metrics/vhosts/{vhost}`
    - `GET /v2/metrics/vhosts/{vhost}/apps/{app}`
    - `GET /v2/metrics/vhosts/{vhost}/apps/{app}/streams/{stream}`
    - `GET /v2/metrics/vhosts/{vhost}/apps/{app}/streams/{stream}/sessions`
  - Added collector filtering via `?collect[]=` and `?exclude[]=` across `core`, `traffic`, `connection`, `stream`, `queue`, `push`, `session`, `frame_latency`, `session_latency`, and `http`.
  - Added end-to-end frame latency span measurement (`<Modules><Measurements><FrameLatency>` with configurable `<Buckets>`, legacy `<Modules><FrameLatency>` fallback, and OVT publisher `<FrameLatency><Enable>` propagation through OVT packets/depacketizer).
  - Added HTTP/1.1 & HTTP/2 server request timing instrumentation (`<Modules><Measurements><HTTPRequestTiming>`) with query-string sanitization.
  - Added TCP/ICE/RTCP SR RTT and track/session metrics across `ov::Socket`, ICE, RTCP SR, WHIP, and publishers/providers (WebRTC, LLHLS, HLS, OVT, SRT, Thumbnail).
  - Added unit and endpoint tests (`openmetrics_endpoint_test.cpp`, `openmetrics_config_test.cpp`, `openmetrics_core_test.cpp`, `openmetrics_features_test.cpp`, `ovt_frame_latency_test.cpp`, `rtcp_sr_rtt_test.cpp`).

- **MPEG-2 TS / SRT Opus audio ingest** (`2f351212`)
  - Added Opus audio demuxing and PES control-header splitting for MPEG-2 TS and SRT providers (`opus_descriptor.h`, `opus_ts_splitter.*`, `mpegts_depacketizer.cpp`, `mpegts_stream.cpp`).
  - Supports mono and stereo Opus streams (e.g., FFmpeg `-c:a libopus -pes_payload_size 0`) with zero-transcode passthrough to WebRTC via `<BypassIfMatch>`; multichannel and dual-mono tracks are skipped with a warning.
  - Updated `docs/live-source/srt.md` and `docs/live-source/mpeg-2-ts-beta.md`, and added unit tests (`opus_ts_splitter_test.cpp`, `mpegts_opus_test.cpp`).

---

## 2026-11-21

### Added
- **WebRTC multi-audio track publishing (`<WebRtcMultiAudioTrack>`)** (`e1bb4922`)
  - Added `<Options><WebRtcMultiAudioTrack>` playlist option (disabled by default) so each rendition sends every track of its audio variant (e.g., multiple languages) as separate `m=audio` sections sharing the video's media stream and CNAME.
  - Extended WebRTC signaling `playlist` notifications with `audio_mids` and per-rendition `audio_tracks`, supporting client-rejected `m=audio` sections (port `0`) and ABR rendition switching.
  - Propagated the option over OVT signaling and multiplex profiles, exposed it in the REST API (`api/ome.yml`, `src/modules/json_serdes/stream.cpp`), updated `misc/conf_examples/Server.xml` and `Origin.xml`, and documented it in `docs/streaming/webrtc-publishing.md`, `docs/transcoding/abr.md`, `docs/live-source/srt.md`, and REST API docs.
  - Added unit tests in `src/modules/sdp/session_description_test.cpp` and `src/publishers/webrtc/webrtc_test.cpp`.

- **Fork GHCR Docker image CI workflow** (`7e7cb362`)
  - Added `.github/workflows/fork-ghcr.yml` (kept separate from upstream workflows to avoid merge conflicts) to build `linux/amd64` images from the local checkout (`USE_LOCAL=true`) on pushes to `ssysm`, `fork-*` tags, and manual dispatches.
  - Fetches upstream tags to stamp `src/main/git_info.h` and publishes images to `ghcr.io/<owner>/ovenmediaengine` (`latest`, `ssysm`, tag, and `sha-*`). Skipped in `OvenMediaLabs`.
