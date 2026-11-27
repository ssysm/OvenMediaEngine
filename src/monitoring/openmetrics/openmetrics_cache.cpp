//==============================================================================
//
//  OvenMediaEngine
//
//  Copyright (c) 2026 AirenSoft. All rights reserved.
//
//==============================================================================
#include "openmetrics_cache.h"

#include <zlib.h>

namespace mon::openmetrics
{
	bool CompressGzip(const std::string &input, std::shared_ptr<ov::Data> *output)
	{
		if (output == nullptr)
		{
			return false;
		}

		z_stream zs{};
		// 15 + 16 enables gzip wrapper
		if (::deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		{
			return false;
		}

		zs.next_in	= reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
		zs.avail_in = static_cast<uInt>(input.size());

		auto result = std::make_shared<ov::Data>();
		result->Reserve(std::max<size_t>(256, input.size() / 4));

		uint8_t out_buf[16384];
		int ret = Z_OK;

		do
		{
			zs.next_out	 = out_buf;
			zs.avail_out = sizeof(out_buf);

			ret = ::deflate(&zs, Z_FINISH);
			if (ret == Z_STREAM_ERROR)
			{
				::deflateEnd(&zs);
				return false;
			}

			size_t have = sizeof(out_buf) - zs.avail_out;
			if (have > 0)
			{
				result->Append(out_buf, have);
			}
		} while (ret != Z_STREAM_END);

		::deflateEnd(&zs);
		*output = std::move(result);
		return true;
	}
}  // namespace mon::openmetrics
