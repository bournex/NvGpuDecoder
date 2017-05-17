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
		list<ISmartFramePtr>	frames;
		boost::recursive_mutex			mtx;
	public:
		PipeQueue(){}
		~PipeQueue(){ 
			cout << "PipeQueue destructing~" << endl;
			frames.clear();
			mtx.destroy();
		}
		void Push(ISmartFramePtr frame) { 
			boost::lock_guard<boost::recursive_mutex> lock(mtx);
			frames.push_back(frame); 
		}
		ISmartFramePtr Pop() { 

			boost::lock_guard<boost::recursive_mutex> lock(mtx);
			if (frames.size())
			{
				ISmartFramePtr p = frames.front();
				frames.pop_front();
				return p;
			}
			return NULL;
		}
	};

	unsigned int					procedure_count;
	unsigned int					procedure_threads;
	boost::thread **				pipeline;
	PipeQueue *						pipequeue;		/* each procedure have a PipeQueue, 
														workers consume frames in there PipeQueue, 
														processed frames push to next procedure queue */
	boost::atomic_bool				eop;			/* end of procedure */
	char *							host_nv12;	/* host nv12 buffer */

public:
	/**
	 * Description: init with "procedure" size of procedures, each procedure has "worker" threads.
	 */
	BatchPipeline(unsigned int procedure = 2, unsigned int worker = 1)
		:procedure_count(procedure), procedure_threads(worker), eop(false), host_nv12(NULL)
	{
		BOOST_ASSERT(procedure > 0);
		BOOST_ASSERT(worker > 0);
		/**
		 * Description: init for device buffer copy
		 */
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

			/* [TODO] delete pipequeue will cause a DEBUG ASSERTION FAILED. unsolved.
			if (pipequeue)
			{
				delete pipequeue;
				pipequeue = NULL;
			}
			*/

			delete pipeline;
			pipeline = NULL;
		}

		if (host_nv12)
		{
			delete host_nv12;
			host_nv12 = NULL;
		}
	}

	void EatBatch(ISmartFramePtr *batch, unsigned int len)
	{
		/**
		 * Description: push to pipeline entry queue
		 */
		for (int i = 0; i < len; i++)
			pipequeue[0].Push(batch[i]);
	}

private:
	void PipelineRoutine(unsigned int pipeindex)
	{
		while (!eop)
		{
			ISmartFramePtr frame(pipequeue[pipeindex].Pop());
			if (frame == NULL)
			{
				boost::this_thread::sleep(boost::posix_time::millisec(10));
				continue;
			}

			/**
			 * Description: do something
			 */
			boost::this_thread::sleep(boost::posix_time::millisec(10));

			if ((pipeindex + 1) != procedure_count)
			{
				/**
				 * Description: push to next procedure queue
				 */
				pipequeue[pipeindex + 1].Push(frame);
			}
			else
			{
				/**
				 * Description: last procedure
				 */
				std::cout << "frame " << frame->FrameNo() << " released~" << std::endl;
			}
		}
	}
};