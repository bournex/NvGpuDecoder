#pragma once
#include <boost/thread.hpp>
#include <map>
#include "MTGpuFramework.h"

using namespace std;

class MtPlayGround
{
private:
	FrameBatchPipe		batchpipe;

public:
	MtPlayGround(FrameBatchRoutine _playcb, void *_invoker, void *cuCtx, bool loop)
		: batchpipe(_playcb, _invoker, cuCtx, 4, 40, loop/*batch size, default 1*/)
	{

	}

	int AddVideo(std::string s)
	{
		return batchpipe.Startup(s);
	}
};