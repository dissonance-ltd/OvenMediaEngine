# Experimental Intel QSV video encoding

The opt-in oneVPL path supports **H.264, HEVC (H.265) and AV1 encoding**
through FFmpeg 7.1.5 `h264_qsv`, `hevc_qsv` and `av1_qsv`.
Ingest decoding and scaling remain on CPU:
`software decoder -> CPU scale/format=nv12 -> QSV encoder -> OME output`.
There is no hardware decode, GPU scaling, zero-copy, 10-bit input or explicit
multi-GPU selection in this implementation.

## Opt-in Docker build

Use the repository checkout and `USE_LOCAL=true`; otherwise the default
Dockerfile fetches upstream OME instead of this fork. Use a new image tag;
building a candidate does not deploy it or restart an existing Origin.

```sh
docker build -t ome:intel-qsv-codecs -f Dockerfile \
  --build-arg USE_LOCAL=true \
  --build-arg USE_INTEL_QSV=true \
  --build-arg BASE_UBUNTU_VERSION=24.04 .
```

This enables `OME_HWACCEL_QSV` and builds bundled FFmpeg with
`--enable-libvpl --enable-vaapi` and all three QSV encoders. Rebuilding only
the OME binary against the older AV1-only FFmpeg is not sufficient.
Intel acceleration is disabled by default; the Intel oneVPL, VAAPI and GPU
runtime packages are installed only for opt-in builds.

The container needs access to the intended Intel render node and the `iHD`
VAAPI driver. For example, adapt the ports and configuration directory before
running a separate test instance:

```sh
docker run --rm --device /dev/dri/renderD128:/dev/dri/renderD128 \
  -e LIBVA_DRIVER_NAME=iHD \
  -v "$PWD/conf:/opt/ovenmediaengine/bin/origin_conf:ro" \
  -p 127.0.0.1:11935:1935 -p 127.0.0.1:13333:3333 \
  ome:intel-qsv-codecs
```

`conf` must contain a complete valid OME configuration. The current
implementation uses the **default oneVPL device**, exposed as `QSV:0`; the
module index is not an explicit `/dev/dri/renderD*` selector. Check the PCI
association of the passed render node on a multi-GPU host. Driver/runtime
versions may need adjustment for the GPU firmware and host kernel.

## Native build

Install oneVPL, libva and libdrm development packages first (on Ubuntu:
`libvpl-dev libva-dev libdrm-dev`). Rebuild FFmpeg prerequisites even if the
installed FFmpeg has the same version but only `av1_qsv` enabled:

```sh
cmake -DOME_HWACCEL_QSV=ON -DTARGET=ffmpeg -P cmake/InstallPrerequisites.cmake
cmake -S . -B build/Release -DCMAKE_BUILD_TYPE=Release -DOME_HWACCEL_QSV=ON
cmake --build build/Release -j4
```

The prerequisite installer installs system packages and libraries; run it
explicitly on the intended build host or in a disposable build container.
A oneVPL GPU runtime (for example `libmfx-gen1.2`), the `iHD` driver and device
permissions are also required at runtime.

## Application/OutputProfiles example

This fragment is not a complete `Server.xml`. Preserve the application's
providers, publishers, access controls and existing output tracks. Add playlist
renditions referring to these video names to make them available in ABR.

```xml
<HWAccels>
  <Decoder><Enable>false</Enable></Decoder>
  <Encoder>
    <Enable>true</Enable>
    <Modules>QSV:0,DEFAULT</Modules>
  </Encoder>
</HWAccels>
<OutputProfile>
  <Name>qsv_test</Name>
  <OutputStreamName>${OriginStreamName}</OutputStreamName>
  <Encodes>
    <Video>
      <Name>h264_720p</Name><Codec>h264</Codec><Modules>QSV:0</Modules>
      <Bitrate>2024000</Bitrate><Width>1280</Width><Height>720</Height>
      <Framerate>30</Framerate><KeyFrameInterval>60</KeyFrameInterval>
      <Preset>medium</Preset><Profile>baseline</Profile><BFrames>0</BFrames>
    </Video>
    <Video>
      <Name>hevc_720p</Name><Codec>h265</Codec><Modules>QSV:0</Modules>
      <Bitrate>2024000</Bitrate><Width>1280</Width><Height>720</Height>
      <Framerate>30</Framerate><KeyFrameInterval>60</KeyFrameInterval>
      <Preset>medium</Preset><Profile>main</Profile><BFrames>0</BFrames>
    </Video>
    <Video>
      <Name>av1_720p</Name><Codec>av1</Codec><Modules>QSV:0</Modules>
      <Bitrate>2500000</Bitrate><Width>1280</Width><Height>720</Height>
      <Framerate>30</Framerate><KeyFrameInterval>60</KeyFrameInterval>
      <Preset>medium</Preset><BFrames>0</BFrames>
    </Video>
    <Audio><Name>audio</Name><Bypass>true</Bypass></Audio>
  </Encodes>
</OutputProfile>
```

For hardware-only verification, pin each video to `QSV:0` and check that the
active codec is `qsv:0,Ready`, not a software fallback. The global module list
is used only when the video does not specify its own `<Modules>`.

- H.264: `QSV:0,DEFAULT` allows software OpenH264 fallback. The default QSV
  profile is `baseline`; `main` and `high` may be selected explicitly.
- AV1: `QSV:0,LIBAOM` allows software AV1 fallback.
- HEVC: use `QSV:0`; this path does not add a software HEVC encoder. The
  default profile is 8-bit `main`, not Main10.
- Presets `faster`, `fast`, `medium`, `slow`, `slower` are forwarded to FFmpeg.
- This low-latency QSV path uses `BFrames=0`. H.264/HEVC forced keyframes are
  requested as IDR frames. Frame- and time-based GOP settings are supported.

## Verification

Build with `OME_BUILD_TESTS=ON` and run `ome_test_transcoder`. QSV tests verify
factory selection, NV12 input, codec-specific output formats, the presence of
all three bundled FFmpeg encoders and preservation of H.264 software modules.
They do not require a GPU and do not establish hardware media correctness.

For hardware acceptance, use a separate loopback-only OME instance with dummy
ingest credentials. Publish synthetic H.264/AAC, require all requested tracks
to report `qsv:0,Ready`, fetch LL-HLS initialization and complete media segments,
and strictly decode each with `ffmpeg -v error -xerror`. Check both the earliest
available and later segments, codec/profile, dimensions, frame rate, monotonic
DTS and an initial keyframe. Keep GPU access distinct from actual encoding:
a QSV-context startup message alone is not proof that an encoder is working.

## Limitations

- Hardware codec availability depends on the selected Intel GPU and driver;
  unsupported encoders fail at initialization, allowing configured fallback.
- NV12 conversion, scaling and memory upload still consume CPU resources.
- Browser codec support, mixed-codec ABR, OVT/remote Edge delivery, missing-GPU
  recovery and multi-hour load/stability require their own acceptance checks.
- Adding these encoders does not automatically migrate or restart deployments.
