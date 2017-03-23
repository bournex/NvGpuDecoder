#pragma once
#include <boost/thread/mutex.hpp>
#include <nvcuvid.h>
#include <iostream>
#include <list>

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define GPU_WIDTH_ALIGN(w) (((w + 511)>>9)<<9)
#define CPU_WIDTH_ALIGN(w) (((w + 3)>>2)<<2)
#define GPU_BUF_CALC(w, h)	(((GPU_WIDTH_ALIGN(w) * h) * 3)>>1)
#define CPU_BUF_CALC(w, h)	(((CPU_WIDTH_ALIGN(w) * h) * 3)>>1)

#define FORMAT_OUTPUT(level, log, ret)	std::cout<<"["<<level<<"] "<<log\
	<<". err("<<ret<<")"<<std::endl;

#define FORMAT_FATAL(log, ret)		FORMAT_OUTPUT("fatal", log, ret)
#define FORMAT_WARNING(log, ret)	FORMAT_OUTPUT("warning", log, ret)
#define FORMAT_INFO(log)			FORMAT_OUTPUT("info", log, 0)

class FramePool
{
private:
	unsigned int polen;
	
	struct FrameData
	{
		unsigned char * data;
		unsigned int	len;
	};

	std::list<FrameData> freelist;
	std::list<FrameData> worklist;

public:
	FramePool(unsigned int len = 8):polen(len)
	{

	}

	~FramePool()
	{

	}

	unsigned char * Alloc(unsigned int len)
	{

	}
};

class GpuDecoder
{
public:
	/**
	 * Description: 
	 */
	GpuDecoder(CUcontext &ctx, CUvideoctxlock &ctxLock, unsigned int devidx = 0, unsigned int queuelen = 8, bool map2host = false)
		: cuCtx(ctx)
		, cuCtxLock(ctxLock)
		, cuParser(NULL)
		, cuDecoder(NULL)
		, cWidth(0)
		, cHeight(0)
		, bMap2Host(map2host)
	{
		int ret = 0;

		if (!cuCtx)
		{
			if (ret = cuInit(0))
			{
				FORMAT_FATAL("create init environment failed", ret);
				throw ret;
			}

			if (ret = cuCtxCreate(&cuCtx, 0, devidx))
			{
				FORMAT_FATAL("create context failed", ret);
				throw ret;
			}
		}

		if (!cuCtxLock && (ret = cuvidCtxLockCreate(&cuCtxLock, cuCtx)))
		{
			FORMAT_FATAL("create context lock failed", ret);
			throw ret;
		}

		/**
		 * Description: create video parser
		 */
		CUVIDPARSERPARAMS videoParserParameters			= {};
		/* my sample only support h264 stream */
		videoParserParameters.CodecType					= cudaVideoCodec_H264;
		/* unknown */
		videoParserParameters.ulMaxNumDecodeSurfaces	= 32;
		/* my sample only support h264 stream */
		videoParserParameters.ulMaxDisplayDelay			= 1;
		/* user data */
		videoParserParameters.pUserData					= this;
		/* callbacks */
		videoParserParameters.pfnSequenceCallback		= HandleVideoSequenceProc;
		videoParserParameters.pfnDecodePicture			= HandlePictureDecodeProc;
		videoParserParameters.pfnDisplayPicture			= HandlePictureDisplayProc;

		if (ret = cuvidCreateVideoParser(&cuParser, &videoParserParameters))
		{
			FORMAT_FATAL("create video parser failed", ret);
			throw ret;
		}
	}

	/**
	 * Description: 
	 */
	~GpuDecoder()
	{
		int ret = 0;

		boost::mutex::scoped_lock (qmtx);
		for (std::list<CuFrame>::iterator it = qpic.begin(); it != qpic.end(); it++)
		{
			if (ret = cuvidUnmapVideoFrame(cuDecoder, it->frame))
			{
				FORMAT_WARNING("unmap video frame failed", ret);
			}
		}

		if (cuParser && (ret = cuvidDestroyVideoParser(cuParser)))
		{
			FORMAT_FATAL("destroy video parser failed", ret);
			throw ret;
		}

		if (cuDecoder && (ret = cuvidDestroyDecoder(cuDecoder)))
		{
			FORMAT_FATAL("destroy video decoder failed", ret);
			throw ret;
		}

		if (cuCtxLock && (ret = cuvidCtxLockDestroy(cuCtxLock)))
		{
			FORMAT_FATAL("destroy context lock failed", ret);
			throw ret;
		}
	}

	/**
	 * Description: global callbacks
	 */
	static int CUDAAPI HandleVideoSequenceProc(void *p, CUVIDEOFORMAT *pVideoFormat);
	static int CUDAAPI HandlePictureDecodeProc(void *p, CUVIDPICPARAMS *pPicParams);
	static int CUDAAPI HandlePictureDisplayProc(void *p, CUVIDPARSERDISPINFO *pDispInfo);

	struct CuFrame
	{
		unsigned int		w;
		unsigned int		h;
		unsigned int		dev_pitch;
		CUdeviceptr			dev_frame;
		unsigned int		host_pitch;		
		unsigned char*		host_frame;
		unsigned long long	timestamp;

		CuFrame(unsigned int _w, unsigned int _h, unsigned int _pitch, CUdeviceptr &_f, unsigned long long _t)
		{
			w			= _w;
			h			= _h;
			dev_pitch	= _pitch;
			dev_frame	= _f;
			host_pitch	= CPU_WIDTH_ALIGN(_w);
			host_frame	= NULL;
			timestamp	= _t;
		}
	};

	inline bool InputStream(unsigned char* pStream, unsigned int nSize)
	{
		if (cuParser)
		{
			int ret = 0;
		
			CUVIDSOURCEDATAPACKET packet = { 0 };
			packet.payload = pStream;
			packet.payload_size = nSize;
			if (!pStream || (nSize == 0))
				packet.flags = CUVID_PKT_ENDOFSTREAM;

			if (ret = cuvidParseVideoData(cuParser, &packet))
			{
				FORMAT_FATAL("parse video data failed", ret);
			}
			else
			{
				return true;
			}
		}

		return false;
	}

	/**
	 * Description: synchronous getting frames
	 */
	inline bool GetFrame(CuFrame &pic)
	{
		boost::mutex::scoped_lock (qmtx);

		if (qpic.size())
		{
			pic = qpic.front();
			qpic.pop_front();
			return true;
		}

		return false;
	}

	inline bool PutFrame(CuFrame &pic)
	{
		int ret = 0;

		boost::mutex::scoped_lock (qmtx);

		if (pic.dev_frame && (ret = cuvidUnmapVideoFrame(cuDecoder, pic.dev_frame)))
		{
			FORMAT_FATAL("unmap video frame failed", ret);
			return false;
		}

		memset(&pic, 0, sizeof(pic));

		return true;
	}

private:
	/**
	 * Description: invoked when video source changed.
	 */
	int HandleVideoSequence(CUVIDEOFORMAT *pVideoFormat)
	{
		int ret = 0;

		/**
		 * Description: video sequence change
		 */
		if (cuDecoder && (ret = cuvidDestroyDecoder(cuDecoder)))
		{
			FORMAT_WARNING("destroy decoder failed", ret);
		}

		cuDecoder = NULL;

		CUVIDDECODECREATEINFO videoDecodeCreateInfo = { 0 };
		memset(&videoDecodeCreateInfo, 0, sizeof(CUVIDDECODECREATEINFO));
		/* codec type */
		videoDecodeCreateInfo.CodecType				= pVideoFormat->codec;

		/* video source resolution */
		videoDecodeCreateInfo.ulWidth				= pVideoFormat->coded_width;
		videoDecodeCreateInfo.ulHeight				= pVideoFormat->coded_height;

		/* currently only support 420 NV12 */
		videoDecodeCreateInfo.ChromaFormat			= pVideoFormat->chroma_format;
		videoDecodeCreateInfo.OutputFormat			= cudaVideoSurfaceFormat_NV12;

		/* adapte with interlacing */
		videoDecodeCreateInfo.DeinterlaceMode		= cudaVideoDeinterlaceMode_Adaptive;

		/* decoded video resolution */
		videoDecodeCreateInfo.ulTargetWidth			= cWidth	= pVideoFormat->coded_width;
		videoDecodeCreateInfo.ulTargetHeight		= cHeight	= pVideoFormat->coded_height;

		/* inner decoded picture cache buffer */
		videoDecodeCreateInfo.ulNumOutputSurfaces	= 4;

		/* using dedicated video engines */
		videoDecodeCreateInfo.ulCreationFlags		= cudaVideoCreate_PreferCUVID;

		/* inner decoding cache buffer */
		videoDecodeCreateInfo.ulNumDecodeSurfaces	= 32;

		/* context lock */
		videoDecodeCreateInfo.vidLock				= cuCtxLock;

		/**
		 * Description: creating decoder
		 */
		if (ret = cuvidCreateDecoder(&cuDecoder, &videoDecodeCreateInfo))
		{
			/**
			 * Description: create decoder failed
			 */
			FORMAT_FATAL("create video decoder failed", ret);
			cuDecoder = NULL;
		}

		return ret;
	}

	/**
	 * Description: invoded when parsed stream data ready.
	 */
	int HandlePictureDecode(CUVIDPICPARAMS *pPicParams)
	{
		if (!cuDecoder)
		{
			FORMAT_WARNING("video decoder not created", -1);
			return -1;
		}

		int ret = 0;
		if (ret = cuvidDecodePicture(cuDecoder, pPicParams))
		{
			FORMAT_FATAL("create video decoder failed", ret);
		}

		return ret;
	}

	/**
	 * Description: invoked when Nv12 data is ready.
	 */
	int HandlePictureDisplay(CUVIDPARSERDISPINFO *pDispInfo)
	{
		do 
		{
			/**
			 * Description: ensure there's room for new picture
			 */
			unsigned int nlen = 0;
			{
				boost::mutex::scoped_lock(qmtx);
				nlen = qpic.size();
			}

			if (nlen < qlen)
				break;	/* free space */
			else
#ifdef WIN32
				Sleep(50);
#else
				usleep(50*1000);
#endif

		} while (1);

		CUVIDPROCPARAMS videoProcessingParameters	= { 0 };
		videoProcessingParameters.progressive_frame = pDispInfo->progressive_frame;
		videoProcessingParameters.second_field		= 0;
		videoProcessingParameters.top_field_first	= pDispInfo->top_field_first;
		videoProcessingParameters.unpaired_field	= (pDispInfo->progressive_frame == 1);

		CUdeviceptr		pSrc	= 0;
		unsigned int	nPitch	= 0;
		
		/**
		 * Description: get decoded frame from inner queue
		 */
		int ret = 0;
		if (ret = cuvidMapVideoFrame(cuDecoder, pDispInfo->picture_index, &pSrc,
			&nPitch, &videoProcessingParameters))
		{
			FORMAT_WARNING("map decoded frame failed", ret);
		}

		boost::mutex::scoped_lock(qmtx);
		qpic.push_back(CuFrame(cWidth, cHeight, nPitch, pSrc, pDispInfo->timestamp));

		return 0;
	}

private:
	CUcontext		cuCtx;		/* context handle */
	CUvideoctxlock	cuCtxLock;	/* context lock */
	CUvideoparser	cuParser;	/* video parser handle */
	CUvideodecoder	cuDecoder;	/* video decoder handle */

	unsigned int	cWidth;		/* current video resolution */
	unsigned int	cHeight;

	boost::mutex				qmtx;
	unsigned int				qlen;		/* cached decoded queue length */
	std::list<CuFrame>			qpic;		/* cached decoded nv12 data */

	bool			bMap2Host;
};

int CUDAAPI GpuDecoder::HandleVideoSequenceProc(void *p, CUVIDEOFORMAT *pVideoFormat)
{return ((GpuDecoder*)p)->HandleVideoSequence(pVideoFormat);}
int CUDAAPI GpuDecoder::HandlePictureDecodeProc(void *p, CUVIDPICPARAMS *pPicParams)
{return ((GpuDecoder*)p)->HandlePictureDecode(pPicParams);}
int CUDAAPI GpuDecoder::HandlePictureDisplayProc(void *p, CUVIDPARSERDISPINFO *pDispInfo)
{return ((GpuDecoder*)p)->HandlePictureDisplay(pDispInfo);}