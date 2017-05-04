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

#include "CircleBatch.h"
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
	unsigned char *			origindata;
	volatile unsigned int	frameno;
	volatile unsigned int	width;
	volatile unsigned int	step;
	volatile unsigned int	height;
	volatile unsigned int	tid;

	volatile unsigned int	batchidx;		/* identify batch sequence */

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
	SmartFramePool(unsigned int poolsize = 2 * 8):quit(false), totalsize(poolsize)
	{
		BOOST_ASSERT(totalsize > 0);

		freefrms.clear();
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
			/**
			 * Description: wait for frames freed
			 */
			boost::this_thread::sleep(boost::posix_time::microseconds(1000));
		} while (BusySize());

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
		boost::lock_guard<boost::mutex> lock(mtx);
		freefrms.push_back(sf);
		busyfrms.erase(sf);

		return 0;
	}

	inline ISmartFrame * Get(unsigned int tid/* who is acquiring frame */)
	{
		if (quit)
		{
			BOOST_ASSERT(false);
			return NULL;
		}

		ISmartFrame * sf = NULL;

		do 
		{
			{
				/**
				 * Description: acquire frame
				 */
				boost::lock_guard<boost::mutex> lock(mtx);
				if (freefrms.size())
				{
					/**
					 * Description: have free frame
					 */
					sf = freefrms.back();
					freefrms.pop_back();
					busyfrms.insert(std::pair<ISmartFrame *, unsigned int>(sf, tid));
					break;
				}
				else if ((freefrms.size() + busyfrms.size()) < totalsize)
				{
					/**
					 * Description: frame lower than pool limit size, create new one
					 */
					sf = new SmartFrame(this);
					busyfrms.insert(std::pair<ISmartFrame *, unsigned int>(sf, tid));
					break;
				}
			}

			/**
			 * Description: wait for free frame
			 */
			boost::this_thread::sleep(boost::posix_time::microseconds(200));

		} while (!sf);

		return sf;
	}

	inline unsigned int FreeSize()
	{
		boost::lock_guard<boost::mutex> lock(mtx);
		return freefrms.size();
	}

	inline unsigned int BusySize()
	{
		boost::lock_guard<boost::mutex> lock(mtx);
		return busyfrms.size();
	}

private:

	boost::atomic_bool						quit;		/* quit flag */

	/**
	 * Description: raw ISmartFrame object pool
	 */
	boost::mutex							mtx;		/* pool lock */
	unsigned int							totalsize;	/* pool max size */
	std::vector<ISmartFrame*>				freefrms;	/* unused frames */
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
					const unsigned int	batch_size	= 8		/* batch init size, equal to or more than threads */,
					const unsigned int	batch_cnt	= 2		/* batch init count, equal to or less than decode queue len */,
					const unsigned int	time_out	= 40	/* millisecond */)
					
		:fbcb(fbroutine), invoker(invk), sfpool(0), batchpipe(fbroutine, invk, batch_size, batch_cnt)
	{
		FORMAT_DEBUG(__FUNCTION__, __LINE__, "constructing FrameBatchPipe");
		BOOST_ASSERT(fbroutine);

		timeout		= min(max(time_out, 1), 50);		/* [1,50]	*/

		sfpool = new SmartFramePool(batch_size * batch_cnt);
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
			 * Description: initialize smart frame
			 */
			static_cast<SmartFrame*>(frame.get())->origindata	= imageGpu;
			static_cast<SmartFrame*>(frame.get())->frameno		= fidx++;
			static_cast<SmartFrame*>(frame.get())->tid			= tid;
			static_cast<SmartFrame*>(frame.get())->step			= s;
			static_cast<SmartFrame*>(frame.get())->height		= h;
			static_cast<SmartFrame*>(frame.get())->width		= w;

			batchpipe.push(frame);
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

		/**
		 * Description: do push
		 */
		batchpipe.push();
	}

	typedef circle_batch<ISmartFramePtr> circle_batch_pipe;

private:
	unsigned int						timeout;		/* timeout interval */
	circle_batch_pipe					batchpipe;		/* batches of SmartFrame */
	boost::thread *						timerthread;	/* timer thread */
	boost::asio::deadline_timer *		deadline;		/* timer object */
	SmartPoolInterface *				sfpool;			/* smart frame pool */
	FrameBatchRoutine					fbcb;			/* frame batch ready callback */
	void *								invoker;		/* callback pointer */

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
