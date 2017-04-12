#pragma once
#include <boost/thread.hpp>
#include "NpMediaSource.h"

typedef void (*PlayWithFrame)(NvCodec::CuFrame &frame, unsigned int tid);

class MtPlayGround
{
private:
	boost::thread **	Workers;
	unsigned int		length;
	boost::atomic_bool	eof;
	PlayWithFrame		playcb;

public:
	MtPlayGround(char **srcvideos, unsigned int len, PlayWithFrame _playcb) : length(len), eof(false), playcb(_playcb)
	{
		/**
		 * Description: init nvidia environment
		 */
		NvCodec::NvCodecInit();

		Workers = new boost::thread *[len];
		for (unsigned int i=0; i<len; i++)
		{
			Workers[i] = new boost::thread(boost::bind(&MtPlayGround::Worker, this, srcvideos[i]));
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

	void Worker(boost::filesystem::path p)
	{
		/**
		 * Description: create media source & decoder
		 */
		NvCodec::NvDecoder							decoder;
		boost::scoped_ptr<NvCodec::NvMediaSource>	media(NULL);
		unsigned int tid = GetCurrentThreadId();

		if (p.extension() == "h264")
		{
			/**
			 * Description: mbf file
			 */
			media.reset(new NvCodec::NvMediaSource(p.string(), &decoder));
		}
		else if (p.extension() == "mbf")
		{
			/**
			 * Description: raw h264 file
			 */
			media.reset(new NvCodec::NpMediaSource(p.string(), &decoder));
		}

		NvCodec::CuFrame frame;

		while (!eof && !media->Eof())
		{
			if (!decoder.GetFrame(frame))
			{
				Sleep(1);
			}
			else
			{
				/**
				* Description: process the nv12 frame
				*/
				if (playcb)
				{
					playcb(frame, tid);
				}

				decoder.PutFrame(frame);
			}
		}
	}
};