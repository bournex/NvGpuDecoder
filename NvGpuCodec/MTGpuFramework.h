#pragma once

#include <thread>
#include <boost/atomic.hpp>
#include <boost/memory_order.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include "FramePool.h"

using namespace boost;

#define PCC_FRAME_MAX_CHAN	4

extern bool Gpu_imageNV12Resize2BGR(
	unsigned char** pSrc,
	unsigned char** pCache,
	unsigned char** pDst,
	int nSrcWidth,
	int nSrcHeight,
	int nSrcPitchW,
	int nSrcPitchH,
	int nDstWidth,
	int nDstHeight,
	int nDstPitchW,
	int nDstPitchH,
	int nBatchSrc);

/**
* Description:	Define the grid program supports image color space.
*/
typedef enum
{
	PCC_CS_GRAY = 0,
	PCC_CS_RGB,
	PCC_CS_BGR,
	PCC_CS_YUV420P,
	PCC_CS_YUV422P,
	PCC_CS_RGBP,
	PCC_CS_BGRP,
} PCC_ColorSpace;

/**
* Description:	Define image type for cloud.
* Members:		width:		Image width in pixels.
*				height:		Image height in pixels.
*				step:		The image step(stride) in bytes of each channel.
*				timeStamp:	Time stamp for marking frame in video.
*				imageData:	Image data pointer of each channel.
*/

struct PCC_Frame
{
	int			   width;
	int			   height;
	int			   step[PCC_FRAME_MAX_CHAN];
	long long	   timeStamp;
	PCC_ColorSpace colorSpace;
	void *		   imageData[PCC_FRAME_MAX_CHAN];
	int			   stepGPU[PCC_FRAME_MAX_CHAN];
	void *		   imageGPU;
};


class GpuFrame
{
public:
	unsigned char *			origindata;
	unsigned char *			thumbdata;
	unsigned int			frameno;
	unsigned int			width;
	unsigned int			step;
	unsigned int			height;
};

class SmartFrame;
typedef boost::intrusive_ptr<SmartFrame>	SmartFramePtr;

class SmartPoolInterface
{
public:
	virtual int Put(SmartFramePtr &sf) = 0;
	virtual int Get(unsigned int tid, SmartFramePtr &sf) = 0;
};

class SmartFrame : public GpuFrame
{
public:
	SmartFrame(SmartPoolInterface *fpool)
		:refcnt(0), sfpool(fpool)
	{
		BOOST_ASSERT(sfpool);
	}

	friend void intrusive_ptr_add_ref(SmartFramePtr sf)
	{
		++sf->refcnt;
	}

	friend void intrusive_ptr_release(SmartFramePtr sf)
	{
		if (--sf->refcnt == 0 && sf->sfpool)
		{
			sf->sfpool->Put(sf);
		}
	}

private:
	boost::atomic_uint32_t	refcnt;
	SmartPoolInterface	*	sfpool;
};

/**
 * Description: fixed size smart frame pool
 */
class SmartFramePool : public SmartPoolInterface
{
public:
	SmartFramePool(unsigned int poolsize = 8 * 8)
	{
		freefrms.resize(poolsize);
	}

	~SmartFramePool()
	{
		/**
		 * Description: cleanup pool items
		 */
	}

	inline int Put(SmartFramePtr &sf)
	{
		freefrms.push_back(sf);
		busyfrms.erase(sf);

		return 0;
	}

	inline int Get(unsigned int tid/* who is acquiring frame */, SmartFramePtr &sf)
	{
		sf = freefrms.back();
		busyfrms.insert(std::pair<SmartFramePtr, unsigned int>(sf, tid));
		freefrms.pop_back();

		return 0;
	}

private:
	std::vector<SmartFramePtr>				freefrms;
	std::map<SmartFramePtr, unsigned int>	busyfrms;	/* save thread tid, which correspond with same decoder */
};

/**
 * Description: parameter of p require thread safe implementation
 */
typedef void (*FrameBatchRoutine)(SmartFramePtr *p, unsigned int len, unsigned int tid, void * invoker);

#define BOUNDARY_SIZE (640 * 320)

class FrameBatchPipe
{
public:
	FrameBatchPipe(	FrameBatchRoutine	fbroutine			/* frame batch ready callback */,
					void *				invk		= 0		/* invoker pointer */,
					unsigned int		batch_size	= 8		/* batch init size, equal to or more than threads */,
					unsigned int		batch_cnt	= 8		/* batch init count, equal to or less than decode queue len */,
					unsigned int		time_out	= 40	/* millisecond */, 
					unsigned int		wscaled		= 640	/* width scaled resolution */,
					unsigned int		hscaled		= 320	/* height scaled resolution */)
					
		:fbcb(fbroutine), invoker(invk), pipeidx(0), winidx(0), sfpool(0), scaledwidth(wscaled), scaledheight(hscaled)
	{
		BOOST_ASSERT(fbroutine);

		batchsize	= batch_size;
		batchcnt	= min(max(batch_cnt, 1), 240);		/* [1,240]	*/
		timeout		= min(max(time_out, 1), 50);		/* [1,50]	*/

		/**
		 * Description: alloc vector buffer
		 */
		batchpipe.resize(batchsize * batchcnt, NULL);

		sfpool = new SmartFramePool(batchsize * batchcnt);
		if (!sfpool)
		{
			throw("create smart frame pool failed");
		}

		timerthread = new boost::thread(boost::bind(&FrameBatchPipe::TimerRoutine, this));
		if (!timerthread)
		{
			throw("create timer thread failed");
		}
	}

	~FrameBatchPipe()
	{
		if (sfpool)
		{
			delete sfpool;
			sfpool = NULL;
		}

		batchpipe.resize(0);
	}
	
	int InputFrame(PCC_Frame *frame, unsigned int tid = 0)
	{
		/**
		 * Description: convert PCC_Frame to SmartFrame
						input SmartFrame to batch pipe
						if reach one batch is full, push batch
		 */

		SmartFramePtr sfptr;
		sfpool->Get(tid, sfptr);
		static const unsigned int pipelen = batchsize * batchcnt;

		if (!sfptr)
		{
			/**
			 * Description: no frame
			 */
			return -1;
		}
		else
		{
			/**
			 * Description: initialize smartframe
			 */
			sfptr->origindata	= (unsigned char *)frame->imageGPU;
			sfptr->frameno		= fidx++;
			sfptr->step			= frame->stepGPU[0];
			sfptr->height		= frame->height;
			sfptr->width		= frame->width;

			do 
			{
				/**
				 * Description: wait to set to batchpipe
				 */
				if (mtxpipe.try_lock())
				{
					if ((pipeidx + 1) != (winidx * batchsize))
					{
						/**
						 * Description: acquired time slice for batchpipe
						 */
						batchpipe[pipeidx++] = sfptr;
						mtxpipe.unlock();
						break;
					}
					mtxpipe.unlock();
				}

				boost::this_thread::sleep(boost::posix_time::microsec(500));

			} while (true);

			boost::mutex::scoped_lock(mtxpipe);
			/**
			 * Description: reach the edge
			 */
			pipeidx = ((++pipeidx == pipelen) ? 0 : pipeidx);

			if ((pipeidx < (winidx * batchsize)) ||				/* '<' out of batch lower bound */
				(pipeidx >= (winidx * batchsize + batchsize)))	/* '>' out of batch upper bound, '=' end of batch */
			{
				/**
				 * Description: active window full
				 */
				if (fbcb)
				{
					fbcb(&batchpipe[winidx * batchsize], batchsize, tid, invoker);
				}

				/**
				 * Description: move active window index 
				 */
				winidx = ((++winidx == batchcnt) ? 0 : winidx);
			}
			else
			{
				/**
				 * Description: 
				 */
			}
		}

		return 0;
	}

private:

#define MAX_FRAME 32
	class FrameBatch
	{
	public:
		unsigned char *origin[MAX_FRAME];
		unsigned char *thumb[MAX_FRAME];
	};

	inline bool Prepare(SmartFramePtr *frames, unsigned int len, FrameBatch &fb)
	{
		unsigned int resolution_of_frame = scaledwidth * scaledheight;
		unsigned int resolution_of_batch = resolution_of_frame * len;

		/**
		 * Description: alloc thumbnail buffer
		 */
		unsigned char * nv12 = smallpool.Alloc(resolution_of_batch);
		unsigned char * bgrp = largepool.Alloc(resolution_of_batch + (resolution_of_batch>>1));
		unsigned char * temp[MAX_FRAME] = { 0 };

		for (int i = 0; i < len; i++)
		{
			fb.origin[i]	= frames[i]->origindata;
			fb.thumb[i]		= (bgrp + i * (resolution_of_frame + (resolution_of_frame>>1)));
			temp[i]			= (nv12 + i * resolution_of_frame);
		}

		if (Gpu_imageNV12Resize2BGR(
			fb.origin, temp, fb.thumb,
			frames[0]->width, frames[0]->height, frames[0]->step, frames[0]->height,
			scaledwidth, scaledheight, scaledwidth,	scaledheight,
			len))
		{
			return false;
		}

		return true;
	}

	void TimerRoutine()
	{
		boost::asio::io_service iosrv; /* io_service object */

		/**
		 * Description: initialize timer with io_service
		 */
		deadline = new boost::asio::deadline_timer(iosrv, boost::posix_time::milliseconds(timeout));
		deadline->async_wait(boost::bind(&FrameBatchPipe::PushPipeTimer, this));
	}

	void PushPipeTimer()
	{
		/**
		 * Description: time's up, reset timer first
		 */
		deadline->expires_at(deadline->expires_at() + boost::posix_time::milliseconds(timeout));
		deadline->async_wait(boost::bind(&FrameBatchPipe::PushPipeTimer, this));

		/**
		 * Description: do push
		 */
	}

private:
	unsigned int						batchsize;		/* one batch size */
	unsigned int						batchcnt;		/* batch count */
	unsigned int						timeout;		/* timeout interval */
	static thread_local unsigned int	fidx;			/* current thread frame index */
	boost::mutex						mtxpipe;		/* batch pipe mutex lock */
	std::vector<SmartFramePtr>			batchpipe;		/* batches of SmartFrame */
	unsigned int						pipeidx;		/* pipe writing index */
	unsigned int						winidx;			/* pipe writing window index */
	boost::thread *						timerthread;	/* timer thread */
	boost::asio::deadline_timer *		deadline;		/* timer object */
	SmartFramePool *					sfpool;			/* smart frame pool */
	FrameBatchRoutine					fbcb;			/* frame batch ready callback */
	void *								invoker;		/* callback pointer */
	unsigned int						scaledwidth;	/* scaled width */
	unsigned int						scaledheight;	/* scaled height */
	FramePool<GpuAllocator>				smallpool;		/* most for NV12 */
	FramePool<GpuAllocator>				largepool;		/* most for BGRP */
};