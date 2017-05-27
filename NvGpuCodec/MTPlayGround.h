#pragma once
#include <boost/thread.hpp>
#include <map>
#include "NpMediaSource.h"
#include "FFCodec.h"


using namespace std;


class MtPlayGround
{
private:
	FrameBatchPipe		batchpipe;

public:
	MtPlayGround(FrameBatchRoutine _playcb, void *_invoker, void *cuCtx)
		: batchpipe(_playcb, _invoker, cuCtx, 4/*batch size, default 1*/)
	{

	}

	int AddVideo(std::string s)
	{
		return batchpipe.Startup(s);
	}
};