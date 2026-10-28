#include <gtest/gtest.h>

#include <modules/ffmpeg/ffmpeg_codec.h>

#include "../../transcoder_encoder.h"

#ifdef HWACCELS_QSV_ENABLED
namespace
{
	struct QsvCodec
	{
		cmn::MediaCodecId id;
		const char *encoder_name;
		cmn::BitstreamFormat bitstream;
	};

	class QsvEncoderTest : public testing::TestWithParam<QsvCodec>
	{
	};
}

TEST_P(QsvEncoderTest, FactoryUsesHardwareEncoderWithNv12Input)
{
	const auto codec = GetParam();
	const info::Stream stream(StreamSourceType::Transcoder);
	auto encoder = TranscodeEncoder::Instantiate(codec.id, cmn::MediaCodecModuleId::QSV, stream);
	ASSERT_NE(encoder, nullptr);
	EXPECT_EQ(encoder->GetCodecID(), codec.id);
	EXPECT_EQ(encoder->GetModuleID(), cmn::MediaCodecModuleId::QSV);
	EXPECT_TRUE(encoder->IsHWAccel());
	EXPECT_EQ(encoder->GetSupportVideoFormat(), cmn::VideoPixelFormatId::NV12);
	EXPECT_EQ(encoder->GetBitstreamFormat(), codec.bitstream);
}

TEST_P(QsvEncoderTest, BundledFfmpegContainsRequestedEncoder)
{
	ffmpeg::FFmpegCodec codec;
	EXPECT_TRUE(codec.AllocEncoderByName(GetParam().encoder_name));
}

INSTANTIATE_TEST_SUITE_P(Intel, QsvEncoderTest, testing::Values(
	QsvCodec{cmn::MediaCodecId::H264, "h264_qsv", cmn::BitstreamFormat::H264_ANNEXB},
	QsvCodec{cmn::MediaCodecId::H265, "hevc_qsv", cmn::BitstreamFormat::H265_ANNEXB},
	QsvCodec{cmn::MediaCodecId::Av1, "av1_qsv", cmn::BitstreamFormat::AV1_OBU}));
#endif

#ifndef HWACCELS_QSV_ENABLED
TEST(QsvEncoderCompatibility, DisabledBuildRejectsQsv)
{
	const info::Stream stream(StreamSourceType::Transcoder);
	for (auto codec : {cmn::MediaCodecId::H264, cmn::MediaCodecId::H265, cmn::MediaCodecId::Av1})
	{
		EXPECT_EQ(TranscodeEncoder::Instantiate(codec, cmn::MediaCodecModuleId::QSV, stream), nullptr);
	}
}
#endif

TEST(QsvEncoderCompatibility, Av1SoftwareModulesRemainAvailable)
{
	const info::Stream stream(StreamSourceType::Transcoder);
	for (auto module : {cmn::MediaCodecModuleId::DEFAULT, cmn::MediaCodecModuleId::LIBAOM})
	{
		auto encoder = TranscodeEncoder::Instantiate(cmn::MediaCodecId::Av1, module, stream);
		ASSERT_NE(encoder, nullptr);
		EXPECT_EQ(encoder->GetModuleID(), cmn::MediaCodecModuleId::LIBAOM);
		EXPECT_FALSE(encoder->IsHWAccel());
		EXPECT_EQ(encoder->GetSupportVideoFormat(), cmn::VideoPixelFormatId::YUV420P);
		EXPECT_EQ(encoder->GetBitstreamFormat(), cmn::BitstreamFormat::AV1_OBU);
	}
}

TEST(QsvEncoderCompatibility, UnsupportedCodecDoesNotFallBackSilently)
{
	const info::Stream stream(StreamSourceType::Transcoder);
	EXPECT_EQ(TranscodeEncoder::Instantiate(cmn::MediaCodecId::Vp8,
		cmn::MediaCodecModuleId::QSV, stream), nullptr);
}

TEST(QsvEncoderCompatibility, H264SoftwareModulesRemainAvailable)
{
	const info::Stream stream(StreamSourceType::Transcoder);
	for (auto module : {cmn::MediaCodecModuleId::DEFAULT, cmn::MediaCodecModuleId::OPENH264,
						cmn::MediaCodecModuleId::X264})
	{
		auto encoder = TranscodeEncoder::Instantiate(cmn::MediaCodecId::H264, module, stream);
		ASSERT_NE(encoder, nullptr);
		EXPECT_FALSE(encoder->IsHWAccel());
		EXPECT_EQ(encoder->GetSupportVideoFormat(), cmn::VideoPixelFormatId::YUV420P);
	}
}
