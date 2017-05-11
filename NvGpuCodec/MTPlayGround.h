#pragma once
#include <boost/thread.hpp>
#include <map>
#include "NpMediaSource.h"

using namespace std;

typedef void (*PlayWithFrame)(ISmartFramePtr *p, unsigned int len, void * invoker);

class MtPlayGround
{
private:
	boost::thread **	Workers;
	unsigned int		length;
	boost::atomic_bool	eof;
	PlayWithFrame		playcb;
	void *				invoker;
	FrameBatchPipe		batchpipe;
	boost::mutex		mtx;
	std::map<void*, NvCodec::NvDecoder*> mpp;

public:
	MtPlayGround(char **srcvideos, unsigned int len, PlayWithFrame _playcb, void *_invoker)
		: length(len), eof(false), playcb(_playcb), invoker(_invoker), batchpipe(OnBatchData, this)
	{
		/**
		 * Description: init nvidia environment
		 */
		BOOST_ASSERT(len > 0);
		NvCodec::NvCodecInit();

		Workers = new boost::thread *[len];
		for (unsigned int i=0; i<len; i++)
		{
			Workers[i] = new boost::thread(boost::bind(&MtPlayGround::Worker, this, boost::filesystem::path(srcvideos[i])));
		}
	}

	~MtPlayGround()
	{
		if (Workers)
		{
			eof = true;

			for (int i=0; i<length; i++)
			{
				if (Workers[i] && Workers[i]->joinable())
				{
					Workers[i]->join();

					delete Workers[i];
					Workers[i] = NULL;
				}
			}

			delete Workers;
			Workers = NULL;
		}
	}

	friend void OnBatchData(ISmartFramePtr *p, unsigned int len, void **expire, unsigned int explen, void * user)
	{
		((MtPlayGround*)user)->BatchData(p, len, expire, explen);
	}

	void BatchData(ISmartFramePtr *p, unsigned int len, void **expire, unsigned int explen)
	{
		/**
		 * Description: return back device decode buffer
		 */
		if (expire && explen)
		{
			for (int i = 0; i < explen; i++)
			{
				// boost::lock_guard<boost::mutex> lk(mtx);
				std::map<void*, NvCodec::NvDecoder*>::iterator it = mpp.find(expire[i]);
				if (it != mpp.end())
				{
					it->second->PutFrame(NvCodec::CuFrame((CUdeviceptr)expire[i]));
					mpp.erase(it);
				}
			}
		}

		if (playcb)
		{
			playcb(p, len, invoker);
		}
	}

	void Worker(boost::filesystem::path p)
	{
		/**
		 * Description: create media source & decoder
		 */
		NvCodec::NvDecoder							decoder(0, 16);
		boost::scoped_ptr<NvCodec::NvMediaSource>	media(NULL);
		unsigned int tid = GetCurrentThreadId();

		if (p.extension() == boost::filesystem::path(".h264"))
		{
			/**
			 * Description: mbf file
			 */
			media.reset(new NvCodec::NvMediaSource(p.string(), &decoder));
		}
		else if (p.extension() == boost::filesystem::path(".mbf"))
		{
			/**
			 * Description: raw h264 file
			 */
			media.reset(new NvCodec::NpMediaSource(p.string(), &decoder));
		}
		else
		{
			/**
			 * Description: unrecognized format
			 */
			return ;
		}

		NvCodec::CuFrame frame;

		FORMAT_DEBUG(__FUNCTION__, __LINE__, "before get frame");
		std::cout << std::boolalpha << eof << media->Eof() << std::endl;
		while (!eof /*&& !media->Eof()*/)
		{
			if (decoder.GetFrame(frame))
			{
				boost::this_thread::sleep(boost::posix_time::milliseconds(10));
			}
			else
			{
				/**
				 * Description: process the nv12 frame
				 */
				batchpipe.InputFrame(frame, tid);

				// boost::lock_guard<boost::mutex> lk(mtx);
				mpp.insert(std::pair<void*, NvCodec::NvDecoder*>((void*)frame.dev_frame, &decoder));
			}
		}
		FORMAT_DEBUG(__FUNCTION__, __LINE__, "after get frame");
	}
};