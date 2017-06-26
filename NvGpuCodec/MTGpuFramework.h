/**
*                                Netposa Video Cloud
*                         (c) Copyright 2011-2017, Netposa
*                                All Rights Reserved
*
* File		: MTGpuFramework.h
* Author	: Liu Xuefei
* Time		: 2017-5-5
*/

#pragma once

#include <thread>
#include <boost/atomic.hpp>
#include <boost/memory_order.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/bind.hpp>
#include <boost/asio.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/lexical_cast.hpp>
#include <algorithm>

#include "CircleBatch.h"
#include "DedicatedPool.h"
#include "SmartFrame.h"
#include "FFCodec.h"

using namespace boost;

class SmartPoolInterface
{
public:
	virtual int Put(ISmartFrame *sf) = 0;
	virtual ISmartFrame * Get(unsigned int tid) = 0;
	virtual ~SmartPoolInterface() {};
};

class SmartFrame : public ISmartFrame
{
public:
	explicit SmartFrame(SmartPoolInterface *fpool)
		:refcnt(0), sfpool(fpool), last(false)
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
		return tid;
	}

	inline bool LastFrame()
	{
		return last;
	}

	inline unsigned long long Timestamp()
	{
		return timestamp;
	}

	inline unsigned int GetRef() const
	{
		unsigned int nnn = this->refcnt.load();
		if (nnn == 3)
		{
			printf("haa\n");
		}
		return this->refcnt.load();
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
	volatile unsigned long long timestamp;
	volatile bool			last;

	BaseCodec*		decoder;

	volatile unsigned int	batchidx;		/* identify batch sequence */

private:
	boost::atomic_uint32_t	refcnt;
	SmartPoolInterface	*	sfpool;
};


class IFrameRestore
{
public:
	virtual void Return(SmartFrame *sf) = 0;
};


/**
* Description: fixed size smart frame pool
*/
class SmartFramePool : public SmartPoolInterface
{
public:
	SmartFramePool(IFrameRestore* prestore, unsigned int poolsize = 1024) :quit(false), totalsize(poolsize), pres(prestore)
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
		pres->Return((SmartFrame*)sf);
		boost::lock_guard<boost::recursive_mutex> lock(mtx);
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
				boost::lock_guard<boost::recursive_mutex> lock(mtx);
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
		boost::lock_guard<boost::recursive_mutex> lock(mtx);
		return freefrms.size();
	}

	inline unsigned int BusySize()
	{
		boost::lock_guard<boost::recursive_mutex> lock(mtx);
		return busyfrms.size();
	}

private:

	boost::atomic_bool						quit;		/* quit flag */
	IFrameRestore *							pres;		/* buffer restore handle */

	/**
	* Description: raw ISmartFrame object pool
	*/
	boost::recursive_mutex					mtx;		/* pool lock */
	unsigned int							totalsize;	/* pool max size */
	std::vector<ISmartFrame*>				freefrms;	/* unused frames */
	std::map<ISmartFrame*, unsigned int>	busyfrms;	/* save thread tid which correspond with same decoder */
};


class FrameBatchPipe : public IFrameRestore
{
public:
	FrameBatchPipe(FrameBatchRoutine fbroutine	/* frame batch ready callback */,
		void *				invk = 0			/* invoker pointer */,
		void *				cuctx = 0			/* cuda context handle */,
		const unsigned int	batch_size = 1		/* batch init size, equal to or more than threads */,
		const unsigned int	time_out = 40		/* millisecond */,
		bool				loop = false)

		:fbcb(fbroutine), invoker(invk), cudactx(cuctx), sfpool(0), batchpipe(OnBatchPop, this, batch_size), decdevpool(512), looplay(loop)
	{
		FORMAT_DEBUG(__FUNCTION__, __LINE__, "constructing FrameBatchPipe");
		BOOST_ASSERT(fbroutine);

		timeout = min(max((int)time_out, 1), 50);		/* [1,50] */

		if (!cudactx)
		{
			/**
			 * Description: no pre-created context, init driver API environment inner
			 */
			NvCodec::NvCodecInit(0, (CUcontext&)cudactx);
		}

		/**
		 * Description: create smart frame pool
		 */
		sfpool = new SmartFramePool(this/* default size 1024 */);
		if (!sfpool)
		{
			throw("create smart frame pool failed");
		}

		/**
		 * Description: start up force push timer thread
		 */
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

	int Startup(std::string &srcvideo)
	{
		// [TODO] increace pool size
		decdevpool.dilation(4);

		BOOST_ASSERT(srcvideo.length());
		boost::thread * t = new boost::thread(boost::bind(&FrameBatchPipe::Worker, this, boost::filesystem::path(srcvideo)));
		BOOST_ASSERT(t);

		tid2parser.insert(std::pair<boost::thread::id, boost::thread*>(t->get_id(), t));
		return 0;
	}

	inline int InputFrame(PCC_Frame *frame, unsigned int tid, BaseCodec* decoder)
	{
		return InputFrame((unsigned char *)frame->imageGPU,
			frame->width, frame->height, frame->stepGPU[0], frame->timeStamp, tid, false /* TODO */, decoder);
	}

	inline int InputFrame(NvCodec::CuFrame &frame, unsigned int tid, BaseCodec* decoder)
	{
		return InputFrame((unsigned char *)frame.dev_frame,
			frame.w, frame.h, frame.dev_pitch, frame.timestamp, frame.last, tid, decoder);
	}

	int InputFrame(unsigned char *imageGpu, unsigned int w, unsigned int h, unsigned int s, unsigned long long t, bool last, unsigned int tid, BaseCodec* decoder)
	{
		/**
		* Description: convert PCC_Frame to SmartFrame
		input SmartFrame to batch pipe
		if reach one batch is full, push batch
		*/
		bool bpush = false;
		{
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
				static_cast<SmartFrame*>(frame.get())->origindata = imageGpu;
				static_cast<SmartFrame*>(frame.get())->frameno = fidx++;
				static_cast<SmartFrame*>(frame.get())->tid = tid;
				static_cast<SmartFrame*>(frame.get())->step = s;
				static_cast<SmartFrame*>(frame.get())->height = h;
				static_cast<SmartFrame*>(frame.get())->width = w;
				static_cast<SmartFrame*>(frame.get())->decoder = decoder;
				static_cast<SmartFrame*>(frame.get())->timestamp = t;
				static_cast<SmartFrame*>(frame.get())->last = last;

				bpush = batchpipe.push(frame);
			}
		}

		if (bpush)
		{
			batchpipe.push_swap();
		}

		return 0;
	}

	inline void Return(SmartFrame *sf)
	{
		NvCodec::CuFrame cuf((void*)sf->NV12());
		sf->decoder->PutFrame(cuf);
	}

private:

	inline void TimerRoutine()
	{
		boost::asio::io_service iosrv; /* io_service object */

		/**
		* Description: initialize timer with io_service
		*/
		deadline = new boost::asio::deadline_timer(iosrv, boost::posix_time::milliseconds(timeout));
		deadline->async_wait(boost::bind(&FrameBatchPipe::PushPipeTimer, this));
	}

	static inline void OnBatchPop(ISmartFramePtr *p, unsigned int nlen, void *user)
	{
		((FrameBatchPipe*)user)->BatchPop(p, nlen);
	}

	inline void BatchPop(ISmartFramePtr *p, unsigned int nlen)
	{
		if (fbcb)
		{
			fbcb(p, nlen, invoker);
		}
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
		// batchpipe.push();
	}

	void Worker(boost::filesystem::path p)
	{
		/**
		* Description: create media source & decoder
		*/
		BaseCodec*			decoder(NULL);
		BaseMediaSource*	media(NULL);

		std::string threadId = boost::lexical_cast<std::string>(boost::this_thread::get_id());
    		unsigned long tid = 0;
    		sscanf(threadId.c_str(), "%lx", &tid);

		if (p.extension() == boost::filesystem::path(".h264"))
		{
			/**
			* Description: raw h264 file
			*/
			decoder = new NvCodec::NvDecoder(0, 4, cudactx, &decdevpool);
			media	= new NvCodec::NvMediaSource(p.string(), decoder, looplay);
		}
		else if (p.extension() == boost::filesystem::path(".mbf"))
		{
			/**
			* Description: mbf file [TODO]
			*/
		}
		else
		{
			/**
			* Description: unrecognized format
			*/
			FFCodec::FFInit();
			decoder = new FFCodec::FFMpegCodec(&decdevpool, cudactx);
			media	= new FFCodec::FFMediaSource(p.string(), decoder);
		}

		NvCodec::CuFrame frame;

		while (!frame.last)
		{
			if (decoder->GetFrame(frame))
			{
				boost::this_thread::sleep(boost::posix_time::microseconds(1000));
			}
			else
			{
				/**
				* Description: process the nv12 frame
				*/
				if (frame.last)
				{
					cout << "end of decoded frame" << endl;
				}
				InputFrame(frame, tid, decoder);
			}
		}
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
	void *								cudactx;		/* cuda context */
	bool								looplay;		/* loop play */
	boost::recursive_mutex				mtx;			/* lock for free device buffer vector */
	DevicePool							decdevpool;		
	std::map<boost::thread::id, boost::thread *>	tid2parser;		/* decoding threads, tid to obj */

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
