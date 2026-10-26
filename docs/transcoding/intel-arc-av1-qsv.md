# Experimental Intel Arc AV1 QSV encoding

This branch adds **AV1 encoding only** through FFmpeg 7.1.5 `av1_qsv`
and Intel oneVPL. H.264 ingest decoding and scaling remain on CPU:
`software decoder -> CPU scale/format=nv12 -> av1_qsv -> OME output`.
No H.264/HEVC QSV, HW decode, zero-copy, or multi-GPU selection.

## Opt-in Docker build

Use the repository checkout and `USE_LOCAL=true`; otherwise the default
Dockerfile fetches the upstream OME source instead of the local fork.

```sh
docker build -t ome:arc-av1-qsv -f Dockerfile \
  --build-arg USE_LOCAL=true \
  --build-arg USE_INTEL_QSV=true \
  --build-arg BASE_UBUNTU_VERSION=24.04 .
```

This builds OME with `-DOME_HWACCEL_QSV=ON` and rebuilds its bundled
FFmpeg with `--enable-libvpl --enable-vaapi` and `av1_qsv`.
Regular images default to Intel acceleration **disabled**. Intel's oneVPL,
VAAPI and media runtime libraries are installed only for opt-in builds.

```sh
docker run --rm --device /dev/dri:/dev/dri \
  -e LIBVA_DRIVER_NAME=iHD \
  -v "$PWD/conf:/opt/ovenmediaengine/bin/origin_conf:ro" \
  -p 1935:1935 -p 9000:9000 \
  ome:arc-av1-qsv
```

Mount a complete valid OME configuration in `conf/Server.xml` and
`conf/Logger.xml`; change the port mappings to fit it.
Ensure the container user can access the Arc render node.
The current prototype uses the **default oneVPL device** and exposes it
as `QSV:0`. It cannot select an explicit `/dev/dri/renderD*` path.
Driver and oneVPL versions in Ubuntu 24.04 may need upgrading depending
on your card's firmware and the host kernel.

## Native build

Install oneVPL headers, libva and libdrm development files first (on
Ubuntu: `libvpl-dev libva-dev libdrm-dev`). Rebuild FFmpeg prerequisites
even if you have already built the same FFmpeg release without QSV:

```sh
cmake -DOME_HWACCEL_QSV=ON -DTARGET=ffmpeg -P cmake/InstallPrerequisites.cmake
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release -DOME_HWACCEL_QSV=ON
cmake --build build/Release -j4
```

A oneVPL **GPU** runtime (e.g. `libmfx-gen1.2`), the `iHD` VAAPI
driver and GPU device access are required **at runtime**.

## Server.xml fragment: Application/OutputProfiles

This is not a complete `Server.xml`. Preserve your Provider, Publisher,
Playlist and VirtualHost configuration.

```xml
<HWAccels>
  <Encoder>
    <Enable>true</Enable>
    <Modules>QSV:0,LIBAOM</Modules>
  </Encoder>
</HWAccels>
<OutputProfile>
  <Name>av1_qsv_test</Name>
  <OutputStreamName>${OriginStreamName}</OutputStreamName>
  <Encodes>
    <Video>
      <Name>av1_720p</Name>
      <Codec>av1</Codec>
      <Modules>QSV:0,LIBAOM</Modules>
      <Bitrate>2500000</Bitrate>
      <Width>1280</Width>
      <Height>720</Height>
      <Framerate>30</Framerate>
      <KeyFrameInterval>60</KeyFrameInterval>
      <BFrames>0</BFrames>
    </Video>
    <Audio>
      <Name>aac_audio</Name>
      <Codec>aac</Codec>
      <Bitrate>128000</Bitrate>
      <Samplerate>48000</Samplerate>
      <Channel>2</Channel>
    </Audio>
  </Encodes>
</OutputProfile>
```

The candidate order `QSV:0,LIBAOM` enables software fallback if the
hardware AV1 encoder fails initialization. For **hardware-only** testing,
remove `,LIBAOM` from `<Modules>`, then verify in logs or monitoring
that the active module is `qsv` and not `libaom`.

## Verification on an actual Intel Arc A380

1. Check `ls -l /dev/dri`; find the Arc device using its PCI association.
   Run `vainfo --display drm --device /dev/dri/renderD128` with the
   correct render-node path and verify an AV1 encoding entry.
2. Start OME with QSV enabled and confirm Intel device detection.
3. Ingest a test 720p30 H.264 stream. Verify QSV encoder creation and AV1
   output, including timestamps and keyframes.
4. Test LL-HLS and OVT delivery with the resulting AV1 output; check
   client playback before enabling it on a public Edge.
5. Exercise no-GPU fallback; long-running stability; several ABR renditions.
6. Verify that encode throughput keeps up with real time, and that delay
   does not grow over a multi-hour stream.

## Explicit limitations

- Experimental **AV1-only** GPU encoder. No GPU decoding or scaling.
- Device discovery only establishes the QSV device context, not AV1
  codec support; unsupported devices are handled at encoder initialization.
- Forcing frames to NV12 incurs CPU processing and memory upload.
- The initial implementation does not guarantee bitstream compatibility
  with every AV1 output container/client; check LL-HLS/OVT in hardware tests.
- Not built or run on a physical Arc A380 in this change.
