#pragma once

#include <thread>
#include <boost/atomic.hpp>
#include <boost/memory_order.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/scoped_ptr.hpp>
#include <algorithm>

#include "DedicatedPool.h"
#include "SmartFrame.h"

using namespace boost;

class SmartPoolInterface
{
public:
	virtual int Put(ISmartFrame *sf)						= 0;
	virtual ISmartFrame * Get(unsigned int tid)				= 0;
	virtual ~SmartPoolInterface() {};
};

class SmartFrame : public ISmartFrame
{
public:
	explicit SmartFrame(SmartPoolInterface *fpool)
		:refcnt(0), sfpool(fpool)
	{
		BOOST_ASSERT(sfpool);
	}

	~SmartFrame()
	{

	}

	inline unsigned char* NV12()
	{
		return origindata;
	}
	inline unsigned int Width()
	{
		return width;
	}
	inline unsigned int Height()
	{
		return height;
	}
	inline unsigned int Step()
	{
		return step;
	}
	inline unsigned int FrameNo()
	{
		return frameno;
	}
	inline unsigned int Tid()
	{
		return 0;
	}

	inline void add_ref(ISmartFrame * sf)
	{
		SmartFrame *ptr = static_cast<SmartFrame*>(sf);
		++ptr->refcnt;
	}

	inline void release(ISmartFrame * sf)
	{
		SmartFrame *ptr = static_cast<SmartFrame*>(sf);
		if ((--ptr->refcnt == 0) && (ptr->sfpool))
		{
			ptr->sfpool->Put(sf);
		}
	}

public:
	unsigned char *					origindata;
	volatile unsigned int			frameno;
	volatile unsigned int			width;
	volatile unsigned int			step;
	volatile unsigned int			height;

	volatile unsigned int			batchidx;		/* identify batch sequence */

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
	SmartFramePool(unsigned int nv12, unsigned int poolsize = 8 * 8):quit(false), totalsize(poolsize)
	{
		BOOST_ASSERT(totalsize > 0);

		resolution_of_nv12 = nv12;
		freefrms.resize(0);
		busyfrms.clear();
	}

	~SmartFramePool()
	{
		/**
		 * Description: cleanup pool items
		 */
		quit = true;

		do 
		{
			if (FreeSize())
			{
				boost::this_thread::sleep(boost::posix_time::microseconds(1000));
			}
			else
			{
				break;
			}
		} while (1);

		typedef std::vector<ISmartFrame*>::iterator VITer;
		for (VITer it = freefrms.begin(); it != freefrms.end(); it++)
		{
			ISmartFrame *p = *it;
			delete p;
			p = NULL;
		}
		freefrms.clear();
	}

	inline int Put(ISmartFrame *sf)
	{
		/**
		 * Description: remove bind to thumbnail buffer
		 */
		boost::mutex::scoped_lock(mtx);
		freefrms.push_back(sf);
		busyfrms.erase(sf);

		return 0;
	}

	inline ISmartFrame * Get(unsigned int tid/* who is acquiring frame */)
	{
		if (quit) return NULL;

		ISmartFrame * sf = NULL;
		while (!FreeSize())
		{
			if (BusySize() < totalsize)
			{
				sf = new SmartFrame(this);
				busyfrms.insert(std::pair<ISmartFrame *, unsigned int>(sf, tid));
				break;
			}
			boost::this_thread::sleep(boost::posix_time::microseconds(200));
		}

		if (!sf)
		{
			boost::mutex::scoped_lock(mtx);
			sf = freefrms.back();
			freefrms.pop_back();
			busyfrms.insert(std::pair<ISmartFrame *, unsigned int>(sf, tid));
		}

		return sf;
	}

	inline unsigned int FreeSize()
	{
		boost::mutex::scoped_lock(mtx);
		return freefrms.size();
	}

	inline unsigned int BusySize()
	{
		boost::mutex::scoped_lock(mtx);
		return busyfrms.size();
	}

private:

	boost::atomic_bool						quit;
	unsigned int							resolution_of_nv12;

	/**
	 * Description: raw ISmartFrame object pool
	 */
	boost::mutex							mtx;
	unsigned int							totalsize;
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

		sfpool = new SmartFramePool(wscaled * hscaled, batchsize * batchcnt);
		if (!sfpool)
		{
			throw("create smart frame pool failed");
		}

		// timerthread = new boost::thread(boost::bind(&FrameBatchPipe::TimerRoutine, this));
		// if (!timerthread)
		//{
		//	throw("create timer thread failed");
		//}
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
			pipeidx = ((pipeidx == pipelen) ? 0 : pipeidx);

			if ((pipeidx < (winidx * batchsize)) ||				/* '<' out of batch lower bound */
				(pipeidx >= (winidx * batchsize + batchsize)))	/* '>' out of batch upper bound, '=' end of batch */
			{
				/**
				 * Description: active window full
				 */

				if (fbcb)
				{
					std::vector<ISmartFramePtr> batchdata;
					batchdata.resize(batchsize);
					std::swap_ranges(
						batchpipe.begin() + (winidx * batchsize), 
						batchpipe.begin() + (winidx * batchsize + batchsize), 
						batchdata.begin());

					fbcb(&batchdata[0], batchsize, invoker);
					batchdata.clear();
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

		if ((pipeidx < (winidx * batchsize)) ||				/* '<' out of batch lower bound */
			(pipeidx >= (winidx * batchsize + batchsize)))	/* '>' out of batch upper bound, '=' end of batch */
		{
			/**
			 * Description: active window full
			 */
			if (fbcb)
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
			if (fbcb)
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

#if (__cplusplus >= 201103L)
	static thread_local unsigned int			fidx;	/* current thread frame index */
#else
	static __declspec(thread) unsigned int		fidx;	/* current thread frame index */
#endif
};

#if (__cplusplus >= 201103L)
thread_local unsigned int FrameBatchPipe::fidx(0);
#else
unsigned int __declspec(thread) FrameBatchPipe::fidx(0);
#endif
