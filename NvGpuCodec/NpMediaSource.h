#pragma once

#include "NvCodec.h"

namespace NvCodec
{
	class NpMediaSource : public NvMediaSource
	{
	public:

		NpMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user, unsigned int cachesize = 1024)
			: NvMediaSource(srcvideo, msdcb, user)
		{
		}

		NpMediaSource(std::string srcvideo, NvDecoder *dec, unsigned int cachesize = 1024)
			: NvMediaSource(srcvideo, dec)
		{
		}

		~NpMediaSource()
		{
		}
	};
}
