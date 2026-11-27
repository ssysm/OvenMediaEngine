//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <cstring>
#include <optional>
#include <vector>

#include "descriptor.h"

namespace mpegts
{
	// Opus in MPEG-TS (the Opus project's ETSI TS draft, as implemented by FFmpeg): a PES private data
	// stream (stream_type 0x06) whose ES descriptors carry
	//   - a registration descriptor (tag 0x05) with format_identifier "Opus", and
	//   - a DVB extension descriptor (tag 0x7F) with descriptor_tag_extension 0x80, whose next byte is
	//     channel_config_code
	class OpusDescriptor
	{
	public:
		static constexpr uint8_t REGISTRATION_DESCRIPTOR_TAG = 0x05;
		static constexpr uint8_t EXTENSION_DESCRIPTOR_TAG	 = 0x7F;
		static constexpr uint8_t OPUS_EXTENSION_TAG			 = 0x80;

		// Whether a registration descriptor identifies the stream as Opus
		static bool IsOpus(const std::vector<std::shared_ptr<Descriptor>> &descriptors)
		{
			for (const auto &descriptor : descriptors)
			{
				if ((descriptor == nullptr) || (descriptor->GetTag() != REGISTRATION_DESCRIPTOR_TAG))
				{
					continue;
				}

				auto data = descriptor->GetData();
				if ((data != nullptr) && (data->GetLength() >= 4) && (::memcmp(data->GetData(), "Opus", 4) == 0))
				{
					return true;
				}
			}

			return false;
		}

		// channel_config_code of the Opus extension descriptor, or nullopt when there is none
		static std::optional<uint8_t> GetChannelConfigCode(const std::vector<std::shared_ptr<Descriptor>> &descriptors)
		{
			for (const auto &descriptor : descriptors)
			{
				if ((descriptor == nullptr) || (descriptor->GetTag() != EXTENSION_DESCRIPTOR_TAG))
				{
					continue;
				}

				auto data = descriptor->GetData();
				if ((data != nullptr) && (data->GetLength() >= 2) && (data->GetDataAs<uint8_t>()[0] == OPUS_EXTENSION_TAG))
				{
					return data->GetDataAs<uint8_t>()[1];
				}
			}

			return std::nullopt;
		}

		// The channel count of a channel_config_code that OME supports: 1 (mono) or 2 (stereo), each one
		// Opus stream as in RTP. 0 for the rest: dual mono (0x00), multichannel (0x03-0x08, 0x81-0x88) and
		// the extended configuration (0xFF) need a multistream decoder setup that OME does not carry.
		static uint32_t GetSupportedChannelCount(uint8_t channel_config_code)
		{
			switch (channel_config_code)
			{
				case 0x01:
					return 1;
				case 0x02:
					return 2;
				default:
					return 0;
			}
		}
	};
}  // namespace mpegts
