#pragma once

#include <boost/thread/mutex.hpp>
#include <boost/foreach.hpp>
#include <algorithm>
#include <iostream>
#include <map>

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
const unsigned int PoolMin = (1<<1);	/* 2 */

/* calculate bounded pool size */
#define BOUNDED_POOLSIZE(poolsize)	poolsize = \
	(std::min(std::max(PoolMin, poolsize), PoolMax))

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
		CUdeviceptr p = NULL;

#ifdef GPU_ALLOC
		if (ret = cuMemAlloc(&p, len))
			FORMAT_FATAL("alloc device buffer failed", ret);
#else
		/**
		 * Description: no implementation
		 */
#endif

		return (void*)p;
	}

	static inline void * Realloc(void *p, unsigned int len)
	{
		BOOST_ASSERT(p);
		BOOST_ASSERT(len);

		int ret = 0;
		p = NULL;

#ifdef GPU_ALLOC
		if (ret = cuMemFree((CUdeviceptr)p))
		{
			FORMAT_FATAL("free device buffer failed", ret);
			return NULL;
		}

		if (ret = cuMemAlloc((CUdeviceptr*)&p, len))
		{
			FORMAT_FATAL("re-alloc device buffer failed", ret);
			return NULL;
		}
#else
		/**
		* Description: no implementation
		*/
#endif

		return (void*)p;
	}

	static inline void	Free(void *p)
	{
		BOOST_ASSERT(p);

#ifdef GPU_ALLOC
		int ret = 0;
		if (ret = cuMemFree((CUdeviceptr)p))
		{
			FORMAT_FATAL("free device buffer failed", ret);
		}
#else
		/**
		* Description: no implementation
		*/
#endif
	}
};

template<class FrameAllocator = CpuAllocator>
class DedicatedPool
{
private:
	/**
	 * Description: pool size
	 */
	unsigned int poolsize;

	/**
	 * Description: swap buffers between two container
	 */
	std::map<unsigned int, unsigned char*> freelist;
	std::map<unsigned char*, unsigned int> worklist;
	boost::mutex	lmtx;

public:
	explicit DedicatedPool(unsigned int len = 8):poolsize(len)
	{
		if (len > PoolMax || len < PoolMin)
			FORMAT_WARNING("pool size is out of range [2, 32768]", len);

		BOUNDED_POOLSIZE(poolsize);
	}

	~DedicatedPool()
	{
		typedef std::map<unsigned int, unsigned char*>::value_type FreeBuf;
		typedef std::map<unsigned char*, unsigned int>::value_type WorkBuf;

		/**
		 * Description: clean up both lists
		 */
		boost::lock_guard<boost::mutex> lock(lmtx);

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

		BOOST_FOREACH(FreeBuf &freebuf, freelist)
		{
			unsigned char *buf = NULL;
			if (freebuf.second)
			{
				buf = freebuf.second;
				FrameAllocator::Free(buf);
				buf = NULL;
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
			boost::lock_guard<boost::mutex> lock(lmtx);

			/**
			 * Description: find proper buffer in freelist
			 */
			for (std::map<unsigned int, unsigned char*>::iterator it = freelist.begin();
				it != freelist.end();
				it ++)
			{
				/**
				 * Description: find buffer in freelist
				 */
				if (it->first >= len)
				{
					buf = it->second;
					worklist.insert(std::pair<unsigned char*, unsigned int>(it->second, it->first));
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
					std::map<unsigned int, unsigned char*>::reverse_iterator it = freelist.rbegin();
					buf = it->second;
					buf = (unsigned char*)FrameAllocator::Realloc((unsigned char*)buf, len);

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

			/**
			 * Description: try until get suitable buffer
			 */

		} while (!buf);

		return buf;
	}

	inline bool Free(unsigned char* buf)
	{
		boost::lock_guard<boost::mutex> lock(lmtx);

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

			freelist.insert(std::pair<unsigned int, unsigned char*>(it->second, it->first));
			worklist.erase(it);
		}

		return true;
	}
};

/**
 * Description: pre-defined pool types
 */
typedef DedicatedPool<CpuAllocator>		HostPool;
typedef DedicatedPool<GpuAllocator>		DevicePool;