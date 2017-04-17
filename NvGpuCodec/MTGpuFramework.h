#pragma once

#include <thread>
#include <boost/atomic.hpp>
#include <boost/memory_order.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/scoped_ptr.hpp>

#include "DedicatedPool.h"
#include "SmartFrame.h"

using namespace boost;

bool Gpu_imageNV12Resize2BGR(
	unsigned char** pSrc, unsigned char** pCache, unsigned char** pDst,
	int nSrcWidth, int nSrcHeight, int nSrcPitchW, int nSrcPitchH,
	int nDstWidth, int nDstHeight, int nDstPitchW, int nDstPitchH,
	int nBatchSrc)
{
	/**
	 * Description: do nothing
	 */
	return true;
}

class SmartPoolInterface
{
public:
	virtual int Put(ISmartFrame *sf)						= 0;
	virtual ISmartFrame * Get(unsigned int tid)				= 0;
	virtual bool Get(unsigned int tid, unsigned char *&)	= 0;
};

class SmartFrame : public ISmartFrame
{
public:
	explicit SmartFrame(SmartPoolInterface *fpool)
		:refcnt(0), sfpool(fpool)
	{
		BOOST_ASSERT(sfpool);
	}

	inline void add_ref(ISmartFrame * sf)
	{
		SmartFrame *ptr = static_cast<SmartFrame*>(sf);
		++ptr->refcnt;
	}

	inline void release(ISmartFrame * sf)
	{
		SmartFrame *ptr = static_cast<SmartFrame*>(sf);
		if ((--ptr->refcnt == 0)
			&& (ptr->sfpool))
		{
			ptr->sfpool->Put(sf);
		}
	}

public:
	unsigned char *			origindata;
	unsigned char *			thumbdata;
	unsigned int			frameno;
	unsigned int			width;
	unsigned int			step;
	unsigned int			height;

	unsigned int			batchidx;		/* identify batch sequence */

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
	SmartFramePool(unsigned int nv12, unsigned int poolsize = 8 * 8):seq(0)
	{
		resolution_of_nv12 = nv12;
		freefrms.resize(poolsize);
	}

	~SmartFramePool()
	{
		/**
		 * Description: cleanup pool items
		 */
	}

	inline int Put(ISmartFrame *sf)
	{
		/**
		 * Description: remove bind to thumbnail buffer
		 */
		BindingPair it = multi_bind_buf.find(static_cast<SmartFrame*>(sf)->batchidx);
		if (it != multi_bind_buf.end())
		{
			MultiBindBuf *mbp = it->second;

			/**
			 * Description: unbind current frame from batch block of bgrp
			 */
			if (!(mbp->flag &= ~(1 << ((sf->BGRP() - mbp->bgr) / resolution_of_nv12))))
			{
				/**
				 * Description: all frames have unbound
				 */
				largepool.Free(mbp->bgr);
				multi_bind_buf.erase(it);
			}
		}

		boost::mutex::scoped_lock(mtx);
		freefrms.push_back(sf);
		busyfrms.erase(sf);

		return 0;
	}

	inline ISmartFrame * Get(unsigned int tid/* who is acquiring frame */)
	{
		boost::mutex::scoped_lock(mtx);
		ISmartFrame * sf = freefrms.back();
		busyfrms.insert(std::pair<ISmartFrame *, unsigned int>(sf, tid));
		freefrms.pop_back();

		return sf;
	}

	inline bool Get(unsigned int batchsize, unsigned char *&bgrp)
	{
		bgrp = largepool.Alloc(batchsize * resolution_of_nv12 + ((batchsize * resolution_of_nv12) >> 1));

		if (bgrp)
		{
			multi_bind_buf.insert(std::pair<unsigned int, MultiBindBuf*>(
				seq++,
				new MultiBindBuf((1 << batchsize) - 1, bgrp)));
		}
		else
		{
			/* fatal */
			largepool.Free(bgrp);
			return false;
		}

		return true;
	}

private:
	struct MultiBindBuf { 
		MultiBindBuf(unsigned int f, unsigned char* p):flag(f), bgr(p) {} 
		unsigned int flag; 
		unsigned char*bgr; 
	};
	typedef std::map<unsigned int, MultiBindBuf *>::iterator	BindingPair;

	/**
	 * Description: since thumb nv12 and bgrp is a large block corresponding with multi frames,
					frame pool should not delete it until all address unbind from it.
	 */
	boost::atomic_uint32_t					seq;
	unsigned int							resolution_of_nv12;
	std::map<unsigned int, MultiBindBuf *>	multi_bind_buf;

	DevicePool								largepool;	/* VRAM pool for BGRP */

	/**
	 * Description: raw ISmartFrame object pool
	 */
	boost::mutex							mtx;
	std::vector<ISmartFrame*>				freefrms;
	std::map<ISmartFrame*, unsigned int>	busyfrms;	/* save thread tid which correspond with same decoder */
};

/**
 * Description: thumbnail default resolution [TODO]
 */
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
		FORMAT_DEBUG(__FUNCTION__, __LINE__, "constructing FrameBatchPipe");
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
		if (deadline)
		{
			boost::system::error_code err;
			deadline->cancel(err);

			if (err)
			{
				/* error occur */
			}

			delete deadline;
			deadline = NULL;
		}

		if (timerthread && timerthread->joinable())
		{
			timerthread->join();
		}

		if (sfpool)
		{
			delete sfpool;
			sfpool = NULL;
		}

		batchpipe.resize(0);
	}
	
	inline int InputFrame(PCC_Frame *frame, unsigned int tid)
	{
		return InputFrame((unsigned char *)frame->imageGPU, 
			frame->width, frame->height, frame->stepGPU[0], tid);
	}

	inline int InputFrame(NvCodec::CuFrame &frame, unsigned int tid)
	{
		return InputFrame((unsigned char *)frame.dev_frame,
			frame.w, frame.h, frame.dev_pitch, tid);
	}

	int InputFrame(unsigned char *imageGpu, unsigned int w, unsigned int h, unsigned int s, unsigned int tid)
	{
		/**
		 * Description: convert PCC_Frame to SmartFrame
						input SmartFrame to batch pipe
						if reach one batch is full, push batch
		 */

		ISmartFramePtr frame(sfpool->Get(tid));
		static const unsigned int pipelen = batchsize * batchcnt;

		if (!frame)
		{
			/**
			 * Description: no frame
			 */
			FORMAT_DEBUG(__FUNCTION__, __LINE__, "get frame failed");
			return -1;
		}
		else
		{
			/**
			 * Description: initialize smartframe
			 */
			static_cast<SmartFrame*>(frame.get())->origindata	= imageGpu;
			static_cast<SmartFrame*>(frame.get())->frameno		= fidx++;
			static_cast<SmartFrame*>(frame.get())->step			= s;
			static_cast<SmartFrame*>(frame.get())->height		= h;
			static_cast<SmartFrame*>(frame.get())->width		= w;

			/* comment shows when batchsize=12/batchcount=4 the pipe looks like */

			/*	  winidx 0	  winidx 1	   winidx 2	   winidx 3		*/
			/*		  ¡ý												*/
			/* +------------+-----------+------------+------------+ */
			/* |############|###		|			 |			  | */
			/* +------------+-----------+------------+------------+ */
			/*		  ¡ý			¡ü									*/
			/*		  ¡ý		pipeidx									*/
			/*		push											*/

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
						batchpipe[pipeidx++] = frame;
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
				FrameBatch fb;


				if (fbcb && Prepare(&batchpipe[winidx * batchsize], batchsize, fb))
				{
					fbcb(&batchpipe[winidx * batchsize], batchsize, invoker);
				}
				else
				{
					FORMAT_FATAL("prepare batch data failed", 0);
					return -1;
				}

				/**
				 * Description: move active window index 
				 */
				winidx = ((++winidx == batchcnt) ? 0 : winidx);
			}
			else
			{
				/**
				 * Description: writing active window, do nothing
				 */
			}
		}

		return 0;
	}

private:

/**
 * Description: adjust max frames to fit pipe size [TODO]
 */
#define MAX_FRAME 32

	class FrameBatch
	{
	public:
		unsigned char *origin[MAX_FRAME];
		unsigned char *thumb[MAX_FRAME];
	};

	class IntermediateNv12
	{
	private:
		unsigned char *		nv12;
		static DevicePool	smallpool;		/* VRAM pool for NV12 */

	public:
		IntermediateNv12(unsigned int size_of_nv12) : nv12(NULL)
		{
			BOOST_ASSERT(nv12 = smallpool.Alloc(size_of_nv12));
		}

		~IntermediateNv12()
		{
			if (nv12)
			{
				smallpool.Free(nv12);
			}
		}

		inline unsigned char *Nv12() { return nv12; }
	};
	/**
	 * Description: make sure calls to Prepare is thread safe
	 */
	bool Prepare(ISmartFramePtr *frames, unsigned int len, FrameBatch &fb)
	{
		/**
		 * Description: thumbnail length
		 */
		unsigned int resolution_of_frame = scaledwidth * scaledheight;
		unsigned int resolution_of_batch = resolution_of_frame * len;

		/**
		 * Description: alloc thumbnail buffer
		 */
		unsigned char * bgrp = NULL;
		unsigned char * nv12 = NULL;
		unsigned char * temp[MAX_FRAME] = { 0 };

		IntermediateNv12 NV12(resolution_of_frame);
		nv12 = NV12.Nv12();

		if (!sfpool->Get(resolution_of_frame, bgrp))
		{
			/* fatal */
			return false;
		}

		for (int i = 0; i < len; i++)
		{
			fb.origin[i]	= static_cast<SmartFrame*>(frames[i].get())->origindata;
			fb.thumb[i]		= (bgrp + i * (resolution_of_frame + (resolution_of_frame>>1)));
			temp[i]			= (nv12 + i * (resolution_of_frame));
		}

		/**
		 * Description: convert nv12 to bgrp
		 */
		return Gpu_imageNV12Resize2BGR(fb.origin, temp, fb.thumb,
			frames[0]->Width(), frames[0]->Height(), frames[0]->Step(), frames[0]->Height(),
			scaledwidth, scaledheight, scaledwidth, scaledheight, len);
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

		static const unsigned int pipelen = batchsize * batchcnt;

		/**
		 * Description: do push
		 */
		boost::mutex::scoped_lock(mtxpipe);

		FrameBatch fb;

		if ((pipeidx < (winidx * batchsize)) ||				/* '<' out of batch lower bound */
			(pipeidx >= (winidx * batchsize + batchsize)))	/* '>' out of batch upper bound, '=' end of batch */
		{
			/**
			 * Description: active window full
			 */
			if (fbcb && Prepare(&batchpipe[winidx * batchsize], batchsize, fb))
			{
				fbcb(&batchpipe[winidx * batchsize], batchsize, invoker);
			}
			else
			{
				FORMAT_FATAL("prepare batch data failed", 0);
			}
		}
		else
		{
			/**
			 * Description: writing active window, force push this window, 
							pipe index pointer move to next window
			 */
			unsigned int pos_in_win = (pipeidx - winidx * batchsize);
			if (fbcb && Prepare(&batchpipe[winidx * batchsize], pos_in_win, fb))
			{
				fbcb(&batchpipe[winidx * batchsize], pos_in_win, invoker);
			}
			else
			{
				FORMAT_FATAL("prepare batch data failed", 0);
			}

			/**
			 * Description: pipe index goto next window directly
			 */
			pipeidx = (((pipeidx + pos_in_win) == pipelen) ? 0 : (pipeidx + pos_in_win));
		}

		/**
		 * Description: move to next window
		 */
		winidx = ((++winidx == batchcnt) ? 0 : winidx);
	}

private:
	unsigned int						batchsize;		/* one batch size */
	unsigned int						batchcnt;		/* batch count */
	unsigned int						timeout;		/* timeout interval */
	static thread_local unsigned int	fidx;			/* current thread frame index */
	boost::mutex						mtxpipe;		/* batch pipe mutex lock */
	std::vector<ISmartFramePtr>			batchpipe;		/* batches of SmartFrame */
	unsigned int						pipeidx;		/* pipe writing index */
	unsigned int						winidx;			/* pipe writing window index */
	boost::thread *						timerthread;	/* timer thread */
	boost::asio::deadline_timer *		deadline;		/* timer object */
	SmartPoolInterface *				sfpool;			/* smart frame pool */
	FrameBatchRoutine					fbcb;			/* frame batch ready callback */
	void *								invoker;		/* callback pointer */
	unsigned int						scaledwidth;	/* scaled width */
	unsigned int						scaledheight;	/* scaled height */
};

thread_local unsigned int FrameBatchPipe::fidx(0);
/**
 * Description: default temp pool size 4 [TODO]
 */
DevicePool FrameBatchPipe::IntermediateNv12::smallpool((unsigned int)4);
