#pragma once
#include <boost/thread/mutex.hpp>
#include <nvcuvid.h>
#include <list>

#include "FramePool.h"

#ifdef WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define GPU_WIDTH_ALIGN(w)	(((w + 511)>>9)<<9)
#define CPU_WIDTH_ALIGN(w)	(((w + 3)>>2)<<2)
#define GPU_BUF_CALC(w, h)	(((GPU_WIDTH_ALIGN(w) * h) * 3)>>1)
#define CPU_BUF_CALC(w, h)	(((CPU_WIDTH_ALIGN(w) * h) * 3)>>1)

#define NV_FAILED	0
#define NV_OK		1

namespace NvCodec
{
	class NvMedia
	{
	public:
		NvMedia()
		{
			FORMAT_INFO("in NvMedia");
		}
		~NvMedia()
		{
			FORMAT_INFO("in ~NvMedia");
		}
	};

	class NvEncoder
	{
	public:
		NvEncoder()
		{
			FORMAT_INFO("in NvEncoder");
		}
		~NvEncoder()
		{
			FORMAT_INFO("in ~NvEncoder");
		}
	};

	class NvDecoder
	{
	public:
		/**
		* Description: 
		*/
		NvDecoder(unsigned int devidx = 0, unsigned int queuelen = 8, bool map2host = true)
			: cuCtx(NULL)
			, cuCtxLock(NULL)
			, cuParser(NULL)
			, cuDecoder(NULL)
			, cWidth(0)
			, cHeight(0)
			, qlen(queuelen)
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
			CUVIDPARSERPARAMS videoParserParameters			= {  };
			/* my sample only support h264 stream */
			videoParserParameters.CodecType					= cudaVideoCodec_H264;
			/* unknown */
			videoParserParameters.ulMaxNumDecodeSurfaces	= (qlen<<1);
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
		~NvDecoder()
		{
			int ret = 0;

			boost::mutex::scoped_lock (qmtx);
			for (std::list<CuFrame>::iterator it = qpic.begin(); it != qpic.end(); it++)
			{
				if (ret = cuvidUnmapVideoFrame(cuDecoder, it->dev_frame))
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

			CuFrame(){}
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
			else
			{
				FORMAT_FATAL("video parser have not created yet", 0);
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

				if (bMap2Host)
				{
					/**
					* Description: alloc & copy to host
				 */
					pic.host_pitch = CPU_WIDTH_ALIGN(pic.w);
					pic.host_frame = framepool.Alloc(pic.host_pitch * pic.h);

					assert(pic.host_frame);

					CUDA_MEMCPY2D d2h	= { 0 };
					d2h.srcMemoryType	= CU_MEMORYTYPE_DEVICE;
					d2h.dstMemoryType	= CU_MEMORYTYPE_HOST;
					d2h.srcDevice		= pic.dev_frame;
					d2h.dstHost			= pic.host_frame;
					d2h.srcPitch		= pic.dev_pitch;
					d2h.dstPitch		= pic.host_pitch;
					d2h.WidthInBytes	= pic.w;
					d2h.Height			= pic.h + (pic.h>>1);

					int ret = 0;

					cuvidCtxLock(cuCtxLock, 0);
					if (ret = cuMemcpy2D(&d2h))
					{
						FORMAT_WARNING("copy to host failed", ret);
					}
					cuvidCtxUnlock(cuCtxLock, 0);
				}

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

			framepool.free(pic.host_frame);
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
			videoDecodeCreateInfo.ulNumOutputSurfaces	= (qlen<<1);

			/* using dedicated video engines */
			videoDecodeCreateInfo.ulCreationFlags		= cudaVideoCreate_PreferCUVID;

			/* inner decoding cache buffer */
			videoDecodeCreateInfo.ulNumDecodeSurfaces	= (qlen<<1);

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

			return ret ? NV_FAILED : NV_OK;
		}

		/**
		* Description: invoded when parsed stream data ready.
		*/
		int HandlePictureDecode(CUVIDPICPARAMS *pPicParams)
		{
			if (!cuDecoder)
			{
				FORMAT_WARNING("video decoder not created", -1);
				return NV_FAILED;
			}

			int ret = 0;
			if (ret = cuvidDecodePicture(cuDecoder, pPicParams))
			{
				FORMAT_FATAL("create video decoder failed", ret);
			}

			return ret ? NV_FAILED : NV_OK;
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

			return ret ? NV_FAILED : NV_OK;
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
		FramePool		framepool;
	};

	int CUDAAPI NvDecoder::HandleVideoSequenceProc(void *p, CUVIDEOFORMAT *pVideoFormat)
	{return ((NvDecoder*)p)->HandleVideoSequence(pVideoFormat);}
	int CUDAAPI NvDecoder::HandlePictureDecodeProc(void *p, CUVIDPICPARAMS *pPicParams)
	{return ((NvDecoder*)p)->HandlePictureDecode(pPicParams);}
	int CUDAAPI NvDecoder::HandlePictureDisplayProc(void *p, CUVIDPARSERDISPINFO *pDispInfo)
	{return ((NvDecoder*)p)->HandlePictureDisplay(pDispInfo);}

}
