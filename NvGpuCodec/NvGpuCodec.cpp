// GpuCodec.cpp : 定义控制台应用程序的入口点。
//

#include <boost/filesystem.hpp>
#include <boost/atomic.hpp>
#include <iostream>
#include <queue>
#include "NvCodec.h"
#include "MTGpuFramework.h"
#include "MTPlayGround.h"

/* multi thread or single thread */
#define MULTITHREAD_FRAMEWORK

using namespace std;

class BatchPipeline
{
private:
	/**
	 * Description: total thread = procedure_count * procedure_threads
	 */
	class PipeQueue
	{
	private:
		queue<ISmartFramePtr>	frames;
		boost::mutex			mtx;
	public:
		void Push(ISmartFramePtr frame) { boost::mutex::scoped_lock(mtx); frames.push(frame); }
		ISmartFramePtr Pop() { boost::mutex::scoped_lock(mtx); ISmartFramePtr p = frames.front(); frames.pop(); return p; }
	};

	unsigned int					procedure_count;
	unsigned int					procedure_threads;
	boost::thread **				pipeline;
	PipeQueue *						pipequeue;
	boost::atomic_bool				eop;			/* end of procedure */

public:
	BatchPipeline(unsigned int procedure = 3, unsigned int worker = 2)
		:procedure_count(procedure), procedure_threads(worker), eop(false)
	{
		pipequeue = new PipeQueue[procedure_count];

		pipeline	= new boost::thread *[procedure_count * procedure_threads];
		for (int i = 0; i < procedure_count; i++)
			for (int j = 0; j < procedure_threads; j++)
				pipeline[i * j] = new boost::thread(boost::bind(&BatchPipeline::PipelineRoutine, this, i));
	}

	~BatchPipeline()
	{
		if (pipeline)
		{
			eop = true;
			for (int i=0; i<(procedure_count * procedure_threads); i++)
			{
				if (pipeline[i] && pipeline[i]->joinable())
				{
					pipeline[i]->join();
					delete pipeline[i];
					pipeline[i] = NULL;
				}
			}

			delete pipeline;
			pipeline = NULL;
		}

		if (pipequeue)
		{
			delete pipequeue;
			pipequeue = NULL;
		}
	}

	void EatBatch(ISmartFramePtr *batch, unsigned int len)
	{
		for (int i = 0; i < len; i++)
			pipequeue[0].Push(batch[i]);
	}

private:
	void PipelineRoutine(unsigned int pipeindex)
	{
		while (!eop)
		{
			ISmartFramePtr frame = pipequeue[pipeindex].Pop();

			/**
			 * Description: do something
			 */
			boost::this_thread::sleep(boost::posix_time::millisec(10));

			if (pipeindex != procedure_count)
			{
				pipequeue[pipeindex + 1].Push(frame);
			}
			else
			{
				/**
				 * Description: last procedure, do nothing, frame destroyed
				 */
			}
		}
	}
};

int main(int argc, char **argv)
{
	BOOST_ASSERT(argc > 1);

#ifdef MULTITHREAD_FRAMEWORK

	auto decodeproc = [](NvCodec::CuFrame &frame, unsigned int tid) {
		auto batchproc = [](ISmartFramePtr *batch, unsigned int len, void *invoker) {
			static BatchPipeline bp;
			bp.EatBatch(batch, len);
		};
		static FrameBatchPipe pipe(batchproc);
		pipe.InputFrame(frame, tid);
	};
	MtPlayGround mvs(&argv[1], argc - 1, decodeproc);

#else

	if (!argv[1] || !boost::filesystem::exists(boost::filesystem::path(argv[1])))
	{
		cout<<"invalid h264 file parameter ...."<<endl<<"GpuCodec.exe path_file_to_decode.h264"<<endl;
		return 0;
	}

	/**
	 * Description: create media source & decoder
	 */
	NvCodec::NvCodecInit();
	NvCodec::NvDecoder decoder;
	NvCodec::NvDecoder::CuFrame frame;
	NvCodec::NvMediaSource media(argv[1], &decoder);

	while (!media.Eof())
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

			decoder.PutFrame(frame);
		}
	}

#endif

	return 0;
}