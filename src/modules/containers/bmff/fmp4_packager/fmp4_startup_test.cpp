//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include <gtest/gtest.h>

#include "duration_boundary_policy.h"
#include "fmp4_test_fixtures.h"

namespace
{
	fmp4_test::TrackPipeline MakeStartupPipeline(const std::shared_ptr<MediaTrack> &track)
	{
		bmff::SegmentBoundaryPolicy::Config config;
		config.segment_duration_ms = 4000;
		config.chunk_duration_ms = 400.0;
		auto policy = std::make_shared<bmff::DurationBoundaryPolicy>(config, false);
		return fmp4_test::MakePipeline(track, std::make_shared<fmp4_test::NullStorageObserver>(), policy, 4000, 400.0, "startup_test");
	}

	std::shared_ptr<MediaPacket> MakeVideoPacket(cmn::MediaCodecId codec, int64_t dts, bool keyframe)
	{
		auto packet = fmp4_test::MakeVideoFrame(0, dts, 100, keyframe);
		if (codec == cmn::MediaCodecId::H265)
		{
			packet->SetBitstreamFormat(cmn::BitstreamFormat::HVCC);
		}
		else if (codec == cmn::MediaCodecId::Av1)
		{
			// A size-delimited frame OBU for the muxer, not a decodable picture.
			const uint8_t obu[] = {0x32, 0x01, 0x00};
			auto data = std::make_shared<ov::Data>(obu, sizeof(obu));
			packet->SetData(data);
			packet->SetBitstreamFormat(cmn::BitstreamFormat::AV1_OBU);
		}
		return packet;
	}

	class FMP4StartupVideoTest : public ::testing::TestWithParam<cmn::MediaCodecId>
	{
	};
}  // namespace

TEST_P(FMP4StartupVideoTest, DropsLeadingDependentFramesBeforeTheFirstKeyframe)
{
	auto track = fmp4_test::MakeVideoTrack(0, 10.0, 10);
	track->SetCodecId(GetParam());
	auto pipeline = MakeStartupPipeline(track);

	// An on-demand OVT pull can join the origin in the middle of a GOP.
	ASSERT_TRUE(pipeline.packager->AppendSample(MakeVideoPacket(GetParam(), 0, false)));
	ASSERT_TRUE(pipeline.packager->AppendSample(MakeVideoPacket(GetParam(), 100, false)));
	ASSERT_TRUE(pipeline.packager->Flush());
	EXPECT_EQ(pipeline.storage->GetSegmentCount(), 0u);

	ASSERT_TRUE(pipeline.packager->AppendSample(MakeVideoPacket(GetParam(), 200, true)));
	ASSERT_TRUE(pipeline.packager->AppendSample(MakeVideoPacket(GetParam(), 300, false)));
	ASSERT_TRUE(pipeline.packager->Flush());

	auto segment = pipeline.storage->GetSegment(0);
	ASSERT_NE(segment, nullptr);
	EXPECT_EQ(segment->GetStartTimestamp(), 200);
	auto partial = pipeline.storage->GetPartialSegment(0, 0);
	ASSERT_NE(partial, nullptr);
	EXPECT_TRUE(partial->IsIndependent());
	EXPECT_DOUBLE_EQ(partial->GetDurationMs(), 200.0);
}

INSTANTIATE_TEST_SUITE_P(VideoCodecs, FMP4StartupVideoTest,
	::testing::Values(cmn::MediaCodecId::H264, cmn::MediaCodecId::H265, cmn::MediaCodecId::Av1));

TEST(FMP4StartupTest, KeepsAnInitialKeyframe)
{
	auto pipeline = MakeStartupPipeline(fmp4_test::MakeVideoTrack(0, 10.0, 10));
	ASSERT_TRUE(pipeline.packager->AppendSample(MakeVideoPacket(cmn::MediaCodecId::H264, 0, true)));
	ASSERT_TRUE(pipeline.packager->Flush());
	auto segment = pipeline.storage->GetSegment(0);
	ASSERT_NE(segment, nullptr);
	EXPECT_EQ(segment->GetStartTimestamp(), 0);
	auto partial = pipeline.storage->GetPartialSegment(0, 0);
	ASSERT_NE(partial, nullptr);
	EXPECT_TRUE(partial->IsIndependent());
}

TEST(FMP4StartupTest, DoesNotWaitForVideoKeyframesOnAnAudioTrack)
{
	auto pipeline = MakeStartupPipeline(fmp4_test::MakeAudioTrack(1));
	auto packet = fmp4_test::MakeAudioFrame(1, 0, 20);
	packet->SetFlag(MediaPacketFlag::NoFlag);
	ASSERT_TRUE(pipeline.packager->AppendSample(packet));
	ASSERT_TRUE(pipeline.packager->Flush());
	auto partial = pipeline.storage->GetPartialSegment(0, 0);
	ASSERT_NE(partial, nullptr);
	EXPECT_EQ(partial->GetStartTimestamp(), 0);
	EXPECT_DOUBLE_EQ(partial->GetDurationMs(), 20.0);
}
