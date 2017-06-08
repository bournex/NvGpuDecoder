#pragma once
#include <string>
#include <boost/atomic.hpp>
#include <boost/assert.hpp>
#include "NvCodecFrame.h"

using namespace std;

class BaseCodec
{
protected:
	boost::atomic_int32_t		qstrategy;	/* queue control strategy */

public:
	virtual int		Init(){ return 0; };
	virtual bool	InputStream(unsigned char* pStream, unsigned int nSize) = 0;
	virtual int		GetFrame(NvCodec::CuFrame &pic)							= 0;
	virtual bool	PutFrame(NvCodec::CuFrame &pic)							= 0;

	enum QueueStrategy
	{
		QSWait = 0,	/* wait until queue has empty place */
		QSPopEarliest = 1,	/* pop the earliest one */
		QSPopLatest = 2,	/* pop the latest one */
		QSMax
	};

	virtual inline int Strategy(int s = -1)
	{
		/* get */
		if (s == -1) return qstrategy;
		BOOST_ASSERT((s >= QSWait) && (s < QSMax));
		/* set */
		return (qstrategy = s);
	}
};

class BaseMediaSource
{
public:
	typedef void(*MediaSrcDataCallback)(unsigned char *data, unsigned int len, void *p);

	BaseMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user, bool looplay = false, unsigned int cachesize = 1024)
		: src(srcvideo), datacb(msdcb), cbpointer(user), decoder(NULL), loop(looplay)
	{
		/* add base class code here */
	}

	BaseMediaSource(std::string srcvideo, BaseCodec *dec, bool looplay = false, unsigned int cachesize = 1024)
		: src(srcvideo), decoder(dec), datacb(NULL), loop(looplay)
	{
		/* add base class code here */
	}

	virtual ~BaseMediaSource()
	{
		/* add base class code here */
	}

	virtual bool Eof()
	{
		return false;
	}

protected:
	BaseCodec *				decoder;	/* NvDecoder object */
	void *					cbpointer;	/* user callback variable */
	MediaSrcDataCallback	datacb;		/* user callback */
	string					src;		/* video source name */
	bool					loop;		/* loop play */
};