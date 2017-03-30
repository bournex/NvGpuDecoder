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

const unsigned int PoolMax = (1<<16);	/* 65536 */
const unsigned int PoolMin = (1<<1);	/* 2 */

#define BOUNDED_POOLSIZE(poolsize)	poolsize = \
	(std::min(std::max(PoolMin, poolsize), PoolMax))

class FramePool
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
	FramePool(unsigned int len = 8):poolsize(len)
	{
		if (len > PoolMax || len < PoolMin)
			FORMAT_WARNING("pool size is out of range [2, 32768]", len);

		BOUNDED_POOLSIZE(poolsize);
	}

	~FramePool()
	{
		typedef std::map<unsigned int, unsigned char*>::value_type FreeBuf;
		typedef std::map<unsigned char*, unsigned int>::value_type WorkBuf;

		/**
		 * Description: clean up both lists
		 */
		boost::mutex::scoped_lock (lmtx);

		BOOST_FOREACH(WorkBuf &workbuf, worklist)
		{
			unsigned char *buf = NULL;
			if (workbuf.first)
			{
				buf = workbuf.first;
				free(buf);
				buf = NULL;
			}
		}

		BOOST_FOREACH(FreeBuf &freebuf, freelist)
		{
			unsigned char *buf = NULL;
			if (freebuf.second)
			{
				buf = freebuf.second;
				free(buf);
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
			boost::mutex::scoped_lock (lmtx);

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
						buf = (unsigned char*)malloc(len);
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
					buf = (unsigned char*)realloc((unsigned char*)buf, len);

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

	inline bool free(unsigned char* buf)
	{
		boost::mutex::scoped_lock (lmtx);

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