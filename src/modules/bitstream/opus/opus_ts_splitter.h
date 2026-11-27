//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 OvenMediaLabs. All rights reserved.
//
//==============================================================================
#pragma once

#include <base/ovlibrary/ovlibrary.h>

#include <cstdint>
#include <vector>

// Splits the payload of an MPEG-TS PES that carries Opus into its Opus packets.
//
// In MPEG-TS (the Opus project's ETSI TS draft, as implemented by FFmpeg), each Opus packet is
// preceded by a control header, and one PES may hold several packets:
//
//   control_header_prefix    11 bits  0x3FF (the first two bytes are 0x7F, 0b111xxxxx)
//   start_trim_flag           1 bit
//   end_trim_flag             1 bit
//   control_extension_flag    1 bit
//   reserved                  2 bits
//   au_size                   0xFF bytes, each adding 255, then a final byte (< 0xFF) adding its value
//   start_trim                16 bits  (3 reserved + 13), if start_trim_flag
//   end_trim                  16 bits  (3 reserved + 13), if end_trim_flag
//   control_extension_length  8 bits, then that many bytes, if control_extension_flag
//   Opus packet               au_size bytes
class OpusTsSplitter
{
public:
	struct Frame
	{
		// The Opus packet without its control header
		size_t offset = 0;
		size_t length = 0;
		// Added to the PES timestamp: the duration of the packets before this one
		int64_t timestamp_offset = 0;
		// The packet's duration in 48 kHz samples
		int samples = 0;
		// Samples (48 kHz) to discard from the start or end of the decoded packet; 0 when not signalled
		uint16_t start_trim = 0;
		uint16_t end_trim = 0;
	};

	struct Result
	{
		// Empty when the payload does not start with a control header; the caller then forwards the
		// payload as one Opus packet, as FFmpeg does
		std::vector<Frame> frames;
		// Why the payload was not split to its end, or empty
		ov::String stop_reason;
	};

public:
	// `timescale` is the track's timebase denominator
	static Result Split(const uint8_t *data, size_t length, int32_t timescale);

	// Whether `data` starts with an Opus control header prefix
	static bool HasControlHeader(const uint8_t *data, size_t length);
};
