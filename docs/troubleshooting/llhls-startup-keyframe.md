# LL-HLS startup after an on-demand OVT pull

## Symptom

A QSV-encoded AV1 stream plays cleanly on Origin and after Edge has been
running, but strict decoding of Edge's first LL-HLS segment fails with
`Error parsing OBU data`. This is a packaging startup defect, not a reason
to enable software AV1 fallback or suppress decoder errors.

## Cause

An on-demand OVT pull can join an existing video stream in the middle of a
GOP. The fMP4 packager previously initialized `_waiting_for_keyframe` to
false. It already dropped dependent video samples after `UpdateTrack()`,
but not when the packager was first constructed. Consequently the first
segment could contain samples that depended on pictures not received by
Edge.

In the captured failing sample, ffprobe reported 42 dependent packets before
the first keyframe. Origin's first segment started with a keyframe. Remuxing
Edge's sample starting at its first keyframe, without re-encoding, made
strict decoding pass. A freshly compiled baseline reproduced the same
failure, ruling out an accidental reliance on the old image's source stamp.

OVT preserves media flags in `ovt_packetizer.cpp` / `ovt_depacketizer.cpp`;
keyframes also remain identifiable in the captured Edge sample. Changing
only the Edge fMP4 startup gate removes the error without changing Origin,
the encoder, parser, GOP, bitrate, or output playlists.

## Fix

Initialize the existing keyframe gate for video tracks in the
`FMP4Packager` constructor. Leading dependent packets are acknowledged and
discarded before conversion, buffering, timestamp reporting, and packaging.
The first accepted video packet is therefore independent. Audio tracks do
not enter this gate, and streams already starting with a keyframe retain it.
Runtime track-change handling is unchanged.

This fixes the shared packager invariant for H.264, H.265 and AV1 rather than
adding an AV1-specific workaround in the OVT or QSV implementation. It does
not force an encoder keyframe or rewrite timestamps. A late-joining Edge
must wait until the next keyframe; a source that never sends one cannot
produce independently decodable video.

## Regression tests

`fmp4_packager/fmp4_startup_test.cpp` drives the real packager and storage:

- Parameterized H.264/H.265/AV1 cases: dependent samples before the first
  keyframe produce no segments, the accepted segment starts at the original
  keyframe timestamp, and its first partial is independent.
- A video track starting with a keyframe keeps that frame.
- An audio sample without the video keyframe flag is still packaged.

The test packets are muxer fixtures, not decodable pictures. Real decoding
is verified separately with generated live media.

With the repository's documented dependencies installed, configure with
`OME_BUILD_TESTS=ON`, then build and run:

```sh
cmake --build build/Release --target ome_test_bmff ome_test_publishers_llhls OvenMediaEngine -j4
build/Release/bin/ome_test_bmff --gtest_filter='*Startup*'
build/Release/bin/ome_test_bmff
build/Release/bin/ome_test_publishers_llhls
```

Verified in a disposable Ubuntu 24.04 build container using the original
QSV runtime image's `/opt/ovenmediaengine` dependencies, GCC 13.3 and
GoogleTest 1.14. No host packages or existing OME containers were replaced.
The three parameterized startup cases failed before the change; all 96
BMFF tests and all 23 LL-HLS tests passed after it.

## Live A/B validation

The isolated Compose project used synthetic RTMP video/audio, the existing
QSV Origin on Intel Arc A380, OVT, and a GPU-less Edge. Baseline and fixed
Edge binaries were compiled with the same toolchain and dependency prefix
and placed in otherwise identical runtime images. Only Edge was swapped.

- Baseline: all 19 steady-state media samples decoded, but the earliest
  Edge AV1 segment failed strict decoding.
- Fixed: all 19 samples plus both Origin/Edge earliest AV1 samples decoded
  without errors; the first packet of the saved Edge startup sample is a
  keyframe. H.264 ABR, AAC radio, LL-HLS fMP4, and TS HLS remain working.

The live harness downloads `EXT-X-MAP` plus a complete fMP4 media segment,
uses ffprobe to require decoded frames, and runs ffmpeg with `-xerror`.
It does not hide the initial segment by checking only a later one.

The original image tag was not overwritten. Candidate local image:
`ome:arc-av1-qsv-startup-fix` (local development build; not a release).

This validation does not establish browser mixed-codec ABR support,
partial-segment/blocking-reload behavior, WAN reconnection, admission
webhooks, DRM, or long-duration stability. The existing BMFF/LL-HLS suites
cover their configured unit scenarios, not those external integrations.
