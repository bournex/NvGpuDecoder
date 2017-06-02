#pragma once

#include <cuda.h>

/**
 * Description: in VRAM, frame size should align to 512 bytes
 */
#define GPU_WIDTH_ALIGN(w)	(((w + 511)>>9)<<9)
#define CPU_WIDTH_ALIGN(w)	(((w + 3)>>2)<<2)
#define GPU_BGR_CALC(w, h)	((GPU_WIDTH_ALIGN(w) * h) * 3)
#define CPU_BGR_CALC(w, h)	((CPU_WIDTH_ALIGN(w) * h) * 3)
#define GPU_NV12_CALC(w, h)	(GPU_BGR_CALC(w, h)>>1)
#define CPU_NV12_CALC(w, h)	(CPU_BGR_CALC(w, h)>>1)

namespace NvCodec
{
	/**
	 * Description: codec frame definition
	 */
	struct CuFrame
	{
		unsigned int		w;			/* width */
		unsigned int		h;			/* height */
		unsigned int		dev_pitch;	/* device frame width pitch */
		void*				dev_frame;	/* device frame buffer */
		unsigned int		host_pitch;	/* host frame width pitch */
		unsigned char*		host_frame;	/* host frame buffer */
		unsigned long long	timestamp;	/* timestamp */
		bool				last;		/* last frame */

		CuFrame() : last(false) {}
		CuFrame(void* _f) : last(false) { dev_frame = _f; host_frame = NULL; }
		CuFrame(unsigned int _w, unsigned int _h, unsigned int _pitch, void* _f, unsigned long long _t, bool _last = false)
		{
			w = _w;
			h = _h;
			dev_pitch = _pitch;
			dev_frame = _f;
			host_pitch = CPU_WIDTH_ALIGN(_w);
			host_frame = NULL;
			timestamp = _t;
			last = _last;
		}
	};
}
