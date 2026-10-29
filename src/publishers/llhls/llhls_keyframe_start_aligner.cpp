//==============================================================================
//
//  OvenMediaEngine
//
//  Created by Getroot
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "llhls_keyframe_start_aligner.h"

#include <base/ovlibrary/ovlibrary.h>

#include <algorithm>

#include "llhls_private.h"

LLHlsKeyframeStartAligner::LLHlsKeyframeStartAligner(int32_t reference_video_track_id,
													 const std::vector<uint32_t> &video_track_ids, const std::vector<uint32_t> &audio_track_ids,
													 size_t max_held_packets, const ov::String &log_context)
	: _reference_video_track_id(reference_video_track_id),
	  _max_held_packets(max_held_packets),
	  _log_context(log_context),
	  _audio_tracks_not_passed(audio_track_ids.begin(), audio_track_ids.end())
{
	for (auto track_id : video_track_ids)
	{
		_video_tracks[track_id] = VideoTrackState();
	}

	if (_reference_video_track_id < 0)
	{
		_audio_state = AudioState::Open;
	}
}

std::vector<std::shared_ptr<MediaPacket>> LLHlsKeyframeStartAligner::Push(const std::shared_ptr<MediaPacket> &media_packet, int64_t dts_us)
{
	std::vector<std::shared_ptr<MediaPacket>> output;

	switch (media_packet->GetMediaType())
	{
		case cmn::MediaType::Video:
			PushVideo(media_packet, dts_us, output);
			break;

		case cmn::MediaType::Audio:
			PushAudio(media_packet, dts_us, output);
			break;

		default:
			output.push_back(media_packet);
			break;
	}

	return output;
}

bool LLHlsKeyframeStartAligner::IsAligned() const
{
	if (_audio_state != AudioState::Open)
	{
		return false;
	}

	for (const auto &[track_id, track_state] : _video_tracks)
	{
		if (track_state.started == false)
		{
			return false;
		}
	}

	// Only a trimmed start has audio to cut after it opened
	return (_start_dts_us.has_value() == false) || _audio_tracks_not_passed.empty();
}

void LLHlsKeyframeStartAligner::PushVideo(const std::shared_ptr<MediaPacket> &media_packet, int64_t dts_us, std::vector<std::shared_ptr<MediaPacket>> &output)
{
	auto track_it = _video_tracks.find(media_packet->GetTrackId());
	if (track_it == _video_tracks.end())
	{
		// Not a track known at the start, so it is not aligned
		output.push_back(media_packet);
		return;
	}

	auto &track_state = track_it->second;
	bool is_reference = (static_cast<int32_t>(media_packet->GetTrackId()) == _reference_video_track_id);

	if (track_state.started == false)
	{
		if (media_packet->IsKeyFrame() == false)
		{
			track_state.dropped_count++;

			if (is_reference == true)
			{
				_audio_state = AudioState::Trimming;
				_newest_video_dts_us = dts_us;
				DropHeldBefore(dts_us);
			}

			return;
		}

		track_state.started = true;

		if (is_reference == true)
		{
			if (_audio_state == AudioState::Trimming)
			{
				_start_dts_us = dts_us;
				DropHeldBefore(dts_us);

				logti("%s The stream joined in the middle of a GOP and starts at the keyframe at %.3f ms (track %d); %zu video and %zu audio packets before it were dropped",
					  _log_context.CStr(), static_cast<double>(dts_us) / 1000.0, _reference_video_track_id, track_state.dropped_count, _dropped_audio_count);
			}

			_audio_state = AudioState::Open;
			ReleaseHeld(output);
		}
		else if (track_state.dropped_count > 0)
		{
			logti("%s Track %u starts at its first keyframe at %.3f ms; %zu packets before it were dropped",
				  _log_context.CStr(), media_packet->GetTrackId(), static_cast<double>(dts_us) / 1000.0, track_state.dropped_count);
		}
	}

	output.push_back(media_packet);
}

void LLHlsKeyframeStartAligner::PushAudio(const std::shared_ptr<MediaPacket> &media_packet, int64_t dts_us, std::vector<std::shared_ptr<MediaPacket>> &output)
{
	switch (_audio_state)
	{
		case AudioState::Holding:
			Hold(media_packet, dts_us);
			break;

		case AudioState::Trimming:
			// Earlier than a frame before the keyframe, so earlier than the keyframe
			if (dts_us < _newest_video_dts_us)
			{
				_dropped_audio_count++;
				break;
			}

			Hold(media_packet, dts_us);
			break;

		case AudioState::Open:
			// Arrived after the start but belongs before it; a track that already
			// passed a packet is never cut again
			if (_start_dts_us.has_value() &&
				_audio_tracks_not_passed.find(media_packet->GetTrackId()) != _audio_tracks_not_passed.end() &&
				dts_us < _start_dts_us.value())
			{
				break;
			}

			_audio_tracks_not_passed.erase(media_packet->GetTrackId());
			output.push_back(media_packet);
			break;
	}
}

void LLHlsKeyframeStartAligner::Hold(const std::shared_ptr<MediaPacket> &media_packet, int64_t dts_us)
{
	// Drop the oldest packet, for OOM protection
	while (_held_audio.empty() == false && _held_audio.size() >= _max_held_packets)
	{
		_held_audio.pop_front();
		_dropped_audio_count++;
	}

	_held_audio.push_back({media_packet, dts_us});
}

void LLHlsKeyframeStartAligner::DropHeldBefore(int64_t dts_us)
{
	auto held_count = _held_audio.size();

	_held_audio.erase(std::remove_if(_held_audio.begin(), _held_audio.end(),
									 [dts_us](const HeldPacket &held) {
										 return held.dts_us < dts_us;
									 }),
					  _held_audio.end());

	_dropped_audio_count += held_count - _held_audio.size();
}

void LLHlsKeyframeStartAligner::ReleaseHeld(std::vector<std::shared_ptr<MediaPacket>> &output)
{
	for (const auto &held : _held_audio)
	{
		_audio_tracks_not_passed.erase(held.packet->GetTrackId());
		output.push_back(held.packet);
	}

	_held_audio.clear();
}
