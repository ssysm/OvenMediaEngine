//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#include "opus_ts_splitter.h"

#include <base/mediarouter/media_type.h>

#include "opus_parser.h"

namespace
{
	constexpr int OPUS_SAMPLE_RATE = 48000;

	// The 11-bit prefix 0x3FF spans the first byte and the top 3 bits of the second
	constexpr uint8_t CONTROL_HEADER_PREFIX_BYTE = 0x7F;
	constexpr uint8_t CONTROL_HEADER_PREFIX_MASK = 0xE0;

	constexpr uint8_t START_TRIM_FLAG			 = 0x10;
	constexpr uint8_t END_TRIM_FLAG				 = 0x08;
	constexpr uint8_t CONTROL_EXTENSION_FLAG	 = 0x04;

	// start_trim and end_trim are 13-bit values behind 3 reserved bits
	constexpr uint16_t TRIM_MASK				 = 0x1FFF;
}  // namespace

bool OpusTsSplitter::HasControlHeader(const uint8_t *data, size_t length)
{
	return (data != nullptr) &&
		   (length >= 2) &&
		   (data[0] == CONTROL_HEADER_PREFIX_BYTE) &&
		   ((data[1] & CONTROL_HEADER_PREFIX_MASK) == CONTROL_HEADER_PREFIX_MASK);
}

OpusTsSplitter::Result OpusTsSplitter::Split(const uint8_t *data, size_t length, int32_t timescale)
{
	Result result;

	if (timescale <= 0)
	{
		result.stop_reason.Format("not split: invalid timescale %d", timescale);
		return result;
	}

	if (HasControlHeader(data, length) == false)
	{
		result.stop_reason = "not split: the payload does not start with an Opus control header";
		return result;
	}

	int64_t total_samples = 0;
	size_t offset		  = 0;

	while (offset < length)
	{
		if (HasControlHeader(data + offset, length - offset) == false)
		{
			result.stop_reason.Format("stopped at offset %zu/%zu with no Opus control header; the input is likely truncated or corrupted", offset, length);
			break;
		}

		const uint8_t flags = data[offset + 1];
		size_t position		= offset + 2;

		// au_size: every 0xFF byte adds 255 and is followed by another size byte
		size_t au_size		= 0;
		bool size_complete	= false;
		while (position < length)
		{
			const uint8_t size_byte = data[position++];
			au_size += size_byte;

			if (size_byte != 0xFF)
			{
				size_complete = true;
				break;
			}
		}

		if (size_complete == false)
		{
			result.stop_reason.Format("stopped at offset %zu: the control header ends before its au_size; the input is likely truncated", offset);
			break;
		}

		Frame frame;

		const size_t trim_bytes = ((flags & START_TRIM_FLAG) ? 2 : 0) + ((flags & END_TRIM_FLAG) ? 2 : 0);
		if ((position + trim_bytes) > length)
		{
			result.stop_reason.Format("stopped at offset %zu: the control header ends before its trim fields; the input is likely truncated", offset);
			break;
		}

		if (flags & START_TRIM_FLAG)
		{
			frame.start_trim = static_cast<uint16_t>(((data[position] << 8) | data[position + 1]) & TRIM_MASK);
			position += 2;
		}

		if (flags & END_TRIM_FLAG)
		{
			frame.end_trim = static_cast<uint16_t>(((data[position] << 8) | data[position + 1]) & TRIM_MASK);
			position += 2;
		}

		if (flags & CONTROL_EXTENSION_FLAG)
		{
			if (position >= length)
			{
				result.stop_reason.Format("stopped at offset %zu: the control header ends before its control extension; the input is likely truncated", offset);
				break;
			}

			const size_t extension_length = data[position++];
			if ((position + extension_length) > length)
			{
				result.stop_reason.Format("stopped at offset %zu: the control extension of %zu bytes exceeds the buffer of %zu bytes", offset, extension_length, length);
				break;
			}
			position += extension_length;
		}

		if (au_size == 0)
		{
			result.stop_reason.Format("stopped at offset %zu: the Opus packet is empty", offset);
			break;
		}

		if ((position + au_size) > length)
		{
			result.stop_reason.Format("stopped at offset %zu: the Opus packet of %zu bytes exceeds the buffer of %zu bytes; the input is likely truncated or corrupted", offset, au_size, length);
			break;
		}

		// A packet that cannot be timed would shift every later packet in the PES, so splitting stops
		const int samples = OPUSParser::GetPacketSamples(data + position, au_size);
		if (samples <= 0)
		{
			result.stop_reason.Format("stopped at offset %zu: the Opus packet has an invalid TOC (0x%02X)", offset, data[position]);
			break;
		}

		frame.offset		   = position;
		frame.length		   = au_size;
		frame.samples		   = samples;
		frame.timestamp_offset = cmn::Rational::Rescale(total_samples, cmn::Rational(1, OPUS_SAMPLE_RATE), cmn::Rational(1, timescale));

		result.frames.push_back(frame);

		total_samples += samples;
		offset = position + au_size;
	}

	return result;
}
