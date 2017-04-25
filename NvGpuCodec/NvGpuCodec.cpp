// GpuCodec.cpp : 定义控制台应用程序的入口点。
//

#include <boost/filesystem.hpp>
#include <boost/atomic.hpp>
#include <iostream>
#include <unordered_map>
#include "NvCodec.h"
#include "MTGpuFramework.h"
#include "MTPlayGround.h"
#include "BatchPipeline.h"

/* pipeline simulation or only decoding */
#define PIPELINE_SIMULATE_FRAMEWORK

using namespace std;

int main(int argc, char **argv)
{	
	BOOST_ASSERT(argc > 1);

#ifdef PIPELINE_SIMULATE_FRAMEWORK

	cout << "start up argument " << argc << endl;

	auto decodeproc = [](NvCodec::CuFrame &frame, unsigned int tid) {
		auto batchproc = [](ISmartFramePtr *batch, unsigned int len, void *invoker) {
			static BatchPipeline bp;
			bp.EatBatch(batch, len);
		};
		static FrameBatchPipe pipe(batchproc);
		pipe.InputFrame(frame, tid);
	};
	MtPlayGround mvs(&argv[1], argc - 1, decodeproc);

	getchar();
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
	NvCodec::CuFrame frame;
	NvCodec::NvMediaSource media(argv[1], &decoder);

	while (!media.Eof())
	{
		if (!decoder.GetFrame(frame))
		{
			boost::this_thread::sleep(boost::posix_time::milliseconds(1));
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