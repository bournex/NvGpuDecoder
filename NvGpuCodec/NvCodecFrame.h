#pragma once

#include <cuda.h>

/**
 * Description: in VRAM, align size is 512 bytes
 */
#define GPU_WIDTH_ALIGN(w)	(((w + 511)>>9)<<9)
#define CPU_WIDTH_ALIGN(w)	(((w + 3)>>2)<<2)
#define GPU_BUF_CALC(w, h)	(((GPU_WIDTH_ALIGN(w) * h) * 3)>>1)
#define CPU_BUF_CALC(w, h)	(((CPU_WIDTH_ALIGN(w) * h) * 3)>>1)

namespace NvCodec
{
	/**
	 * Description: codec frame definition
	 */
	struct CuFrame
	{
		unsigned int		w;
		unsigned int		h;
		unsigned int		dev_pitch;
		CUdeviceptr			dev_frame;
		unsigned int		host_pitch;
		unsigned char*		host_frame;
		unsigned long long	timestamp;

		CuFrame() {}
		CuFrame(unsigned int _w, unsigned int _h, unsigned int _pitch, CUdeviceptr _f, unsigned long long _t)
		{
			w = _w;
			h = _h;
			dev_pitch = _pitch;
			dev_frame = _f;
			host_pitch = CPU_WIDTH_ALIGN(_w);
			host_frame = NULL;
			timestamp = _t;
		}
	};
}
