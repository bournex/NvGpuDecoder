#pragma once

#include <boost/thread/mutex.hpp>
#include <iostream>
#include <map>

#define FORMAT_OUTPUT(level, log, ret)	std::cout<<"["<<level<<"] "<<log\
	<<". err("<<ret<<")"<<std::endl;
#define FORMAT_FATAL(log, ret)		FORMAT_OUTPUT("fatal", log, ret)
#define FORMAT_WARNING(log, ret)	FORMAT_OUTPUT("warning", log, ret)
#define FORMAT_INFO(log)			FORMAT_OUTPUT("info", log, 0)

class FramePool
{
private:
	unsigned int poolmax;

	struct FrameData
	{
		unsigned char * data;
		unsigned int	len;

		FrameData(unsigned char *_data, unsigned int _len):data(_data), len(_len){}
	};

	std::map<unsigned int, unsigned char*> freelist;
	std::map<unsigned char*, unsigned int> worklist;

	boost::mutex	lmtx;


public:
	FramePool(unsigned int len = 8):poolmax(len)
	{
		if (poolmax > 32768)
			FORMAT_WARNING("pool size is too large", poolmax)
	}

	~FramePool()
	{

	}

	inline unsigned char * Alloc(unsigned int len)
	{
		unsigned char *buf = NULL;
		do 
		{
			boost::mutex::scoped_lock (lmtx);
			if (worklist.size() < poolmax)
			{
				for (std::map<unsigned int, unsigned char*>::iterator it = freelist.begin();
					it != freelist.end();
					it ++)
				{
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
					buf = new unsigned char[len];
					worklist.insert(std::pair<unsigned char*, unsigned int>(buf, len));
				}
			}

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
			freelist.insert(std::pair<unsigned int, unsigned char*>(it->second, it->first));
			worklist.erase(it);
		}
		return true;
	}
};