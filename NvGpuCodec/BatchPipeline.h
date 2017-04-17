#pragma once

#include <boost/thread.hpp>
#include <boost/atomic.hpp>
#include <queue>
#include "SmartFrame.h"
using namespace std;

/**
 * Description: pipeline simulation
 */
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

		pipeline = new boost::thread *[procedure_count * procedure_threads];
		for (int i = 0; i < procedure_count; i++)
			for (int j = 0; j < procedure_threads; j++)
				pipeline[procedure_threads * i + j] = new boost::thread(boost::bind(&BatchPipeline::PipelineRoutine, this, i));
	}

	~BatchPipeline()
	{
		if (pipeline)
		{
			eop = true;
			for (int i = 0; i<(procedure_count * procedure_threads); i++)
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
		/**
		 * Description: push to pipeline entry queue
		 */
		cout << "receive batch data " << batch << " size " << len << endl;
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

			if ((pipeindex + 1) != procedure_count)
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