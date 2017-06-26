#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/foreach.hpp>
#include <algorithm>
#include <iostream>
#include <list>
#include <map>
#include "cuda_runtime_api.h"

#define FORMAT_OUTPUT(level, log, ret)	std::cout<<"["<<level<<"] "<<log\
	<<". err("<<ret<<")"<<std::endl;
#define FORMAT_FATAL(log, ret)		FORMAT_OUTPUT("fatal", log, ret)
#define FORMAT_WARNING(log, ret)	FORMAT_OUTPUT("warning", log, ret)
#define FORMAT_INFO(log)			FORMAT_OUTPUT("info", log, 0)
#define FORMAT_FUNCLINE				__FUNCTION__, __LINE__

// #ifdef _DEBUG
#define FORMAT_DEBUG(func, line, log) {char szdbg[1024] = {0};\
	std::cout<<"[debug]["<<func<<"("<<line<<")"<<"] "<<log<<std::endl;}
//#else
//#define FORMAT_DEBUG(func, line, log)
//#endif

/* pool size bound definition */
const unsigned int PoolMax = (1<<16);	/* 65536 */
const unsigned int PoolMin = (1 << 1);	/* 2 */

/* calculate bounded pool size */
#define BOUNDED_POOLSIZE(poolsize)	\
	poolsize = ((PoolMin > poolsize) ? PoolMin : poolsize);\
	poolsize = ((PoolMax > poolsize) ? poolsize : PoolMax);

/**
 * Description: host RAM allocator
 */
class CpuAllocator
{
public:
	static inline void * Malloc(unsigned int len)
	{
		BOOST_ASSERT(len);

		return ::malloc(len);
	}

	static inline void * Realloc(void *p, unsigned int len)
	{
		BOOST_ASSERT(p);
		BOOST_ASSERT(len);

		return ::realloc(p, len);
	}

	static inline void	Free(void *p)
	{
		BOOST_ASSERT(p);

		::free(p);
	}
};

/**
 * Description: nvidia device RAM allocator
 */
class GpuAllocator
{
public:
	static inline void * Malloc(unsigned int len)
	{
		BOOST_ASSERT(len);

		int ret = 0;
		void * p;

		if (ret = cudaMalloc((void**)&p, len))
			FORMAT_FATAL("alloc device buffer failed", ret);

		return (void*)p;
	}

	static inline void * Realloc(void *p, unsigned int len)
	{
		BOOST_ASSERT(p);
		BOOST_ASSERT(len);

		int ret = 0;
		if (ret = cudaFree(p))
		{
			FORMAT_FATAL("free device buffer failed", ret);
			return NULL;
		}

		p = NULL;
		if (ret = cudaMalloc((void**)&p, len))
		{
			FORMAT_FATAL("re-alloc device buffer failed", ret);
			return NULL;
		}

		return (void*)p;
	}

	static inline void	Free(void *p)
	{
		BOOST_ASSERT(p);

		int ret = 0;
		if (ret = cudaFree(p))
		{
			FORMAT_FATAL("free device buffer failed", ret);
		}
	}
};

template<class FrameAllocator = CpuAllocator>
class DedicatedPool
{
private:
	/**
	 * Description: pool size
	 */
	volatile unsigned int poolsize;

	/**
	 * Description: swap buffers between two container
	 */
	// std::map<unsigned int, unsigned char*> freelist;
	class FreeOnes
	{
	public:
		unsigned char* buf;
		unsigned int len;
		FreeOnes(unsigned char*_buf, unsigned int _len) : buf(_buf), len(_len) {}
	};
	std::list<FreeOnes>	freelist;
	std::map<unsigned char*, unsigned int> worklist;
	boost::recursive_mutex	lmtx;

public:
	DedicatedPool(unsigned int len = 32) : poolsize(len)
	{
		if (len > PoolMax || len < PoolMin)
			FORMAT_WARNING("pool size is out of range [2, 32768]", len);

		BOUNDED_POOLSIZE(poolsize);
	}

	~DedicatedPool()
	{
		typedef std::map<unsigned char*, unsigned int>::value_type WorkBuf;

		/**
		 * Description: clean up both lists
		 */
		boost::lock_guard<boost::recursive_mutex> lock(lmtx);

		BOOST_FOREACH(WorkBuf &workbuf, worklist)
		{
			unsigned char *buf = NULL;
			if (workbuf.first)
			{
				buf = workbuf.first;
				FrameAllocator::Free(buf);
				buf = NULL;
			}
		}

		BOOST_FOREACH(FreeOnes &freebuf, freelist)
		{
			unsigned char *buf = NULL;
			if (freebuf.buf)
			{
				FrameAllocator::Free(freebuf.buf);
				freebuf.buf = NULL;
			}
		}

		/**
		 * Description: cleanup buffer list
		 */
		worklist.clear();
		freelist.clear();
	}

	inline unsigned char * Alloc(unsigned int len)
	{
		unsigned char *buf = NULL;
		do 
		{
			/**
			 * Description: find proper buffer in freelist
			 */
			if (lmtx.try_lock())
			{
				for (typename std::list<FreeOnes>::iterator it = freelist.begin();
					it != freelist.end();
					it++)
				{
					/**
					* Description: find buffer in freelist
					*/
					if (it->len >= len)
					{
						buf = it->buf;
						worklist.insert(std::pair<unsigned char*, unsigned int>(it->buf, it->len));
						freelist.erase(it);

						break;
					}
				}

				if (!buf)
				{
					if ((freelist.size() + worklist.size()) < poolsize)
					{
						/**
						* Description: no proper size buffer, alloc heap memory
						*/
						if (!buf)
						{
							buf = (unsigned char*)FrameAllocator::Malloc(len);
							worklist.insert(std::pair<unsigned char*, unsigned int>(buf, len));
						}
					}
					else if (freelist.size())
					{
						/**
						* Description: no suitable free buffer, realloc the biggest one
						*/
						typename std::list<FreeOnes>::reverse_iterator it = freelist.rbegin();
						buf = (unsigned char*)FrameAllocator::Realloc((unsigned char*)it->buf, len);

						BOOST_ASSERT(buf);

						/**
						* Description: enqueue worklist, dequeue freelist
						*/
						worklist.insert(std::pair<unsigned char*, unsigned int>(buf, len));
						freelist.erase(it.base());
					}
					else
					{
						/**
						* Description: worklist full, wait for next around,[TODO liuxf] reduce cpu usage
						*/
					}
				}
				lmtx.unlock();
			}

			boost::this_thread::sleep(boost::posix_time::microseconds(1500));
			/**
			 * Description: try until get suitable buffer
			 */

		} while (!buf);

		//printf("[statistics] tid = %d, alloc = %d\n", GetCurrentThreadId(), ++aidx);
		return buf;
	}

	inline bool Free(unsigned char* buf)
	{
		boost::lock_guard<boost::recursive_mutex> lock(lmtx);

		std::map<unsigned char*, unsigned int>::iterator it = worklist.find(buf);
		if (it == worklist.end())
		{
			FORMAT_WARNING("buffer unrecognized", 0);
			return false;
		}
		else
		{
			/**
			 * Description: return buffer to freelist
			 */
			freelist.push_front(FreeOnes(it->first, it->second));
			worklist.erase(it);
		}

		return true;
	}

	inline unsigned int dilation(unsigned int addition)
	{
		/**
		 * Description: dilate addition size
		 */
		return poolsize += addition;
	}

	//boost::atomic_uint32_t aidx = 0;
	//boost::atomic_uint32_t fidx = 0;
};

typedef DedicatedPool<CpuAllocator>		HostPool;
typedef DedicatedPool<GpuAllocator>		DevicePool;
