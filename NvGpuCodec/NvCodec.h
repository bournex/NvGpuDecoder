#pragma once


#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>
#include <boost/atomic.hpp>
#include <nvcuvid.h>
#include <string>
#include <list>

#include "DedicatedPool.h"
#include "NvCodecFrame.h"

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define NV_FAILED	0
#define NV_OK		1

namespace NvCodec
{
	/**
	 * Description: this pair of function should be call before/after any NvCodec code
	 */
	int NvCodecInit()
	{
		int ret = 0;

		if (ret = cuInit(0))
		{
			FORMAT_FATAL("create init environment failed", ret);
			return ret;
		}

		return 0;
	}

	int NvCodecUninit()
	{
		/* nothing todo right now */
		return 0;
	}

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
		NvDecoder(unsigned int devidx = 0, unsigned int queuelen = 8, bool map2host = false)
			: cuCtx(NULL)
			, cuCtxLock(NULL)
			, cuParser(NULL)
			, cuDecoder(NULL)
			, cWidth(0)
			, cHeight(0)
			, qlen(queuelen)
			, bMap2Host(map2host)
			, qstrategy(QSWait)
		{
			int ret = 0;

			if (!cuCtx)
			{
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
			/* stream cached length */
			videoParserParameters.ulMaxNumDecodeSurfaces	= (qlen<<1);
			/* delay for 1 */
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

				// std::cout<<"address "<<std::setbase(16)<<pStream<<", datalen "<<nSize<<std::endl;
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
		inline int GetFrame(CuFrame &pic)
		{
			{
				boost::mutex::scoped_lock(qmtx);
				if (qpic.size())
				{
					pic = qpic.front();
					qpic.pop_front();
				}
				else
				{
					return -1;
				}
			}

			if (bMap2Host)
			{
				/**
				 * Description: alloc & copy to host
				 */
				pic.host_pitch = CPU_WIDTH_ALIGN(pic.w);
				pic.host_frame = framepool.Alloc(pic.dev_pitch * pic.h + ((pic.dev_pitch * pic.h) >> 1));

				BOOST_ASSERT(pic.host_frame);

				CUDA_MEMCPY2D d2h = { 0 };
				d2h.srcMemoryType = CU_MEMORYTYPE_DEVICE;
				d2h.dstMemoryType = CU_MEMORYTYPE_HOST;
				d2h.srcDevice = pic.dev_frame;
				d2h.dstHost = pic.host_frame;
				d2h.dstDevice = (CUdeviceptr)pic.host_frame;
				d2h.srcPitch = pic.dev_pitch;
				d2h.dstPitch = pic.w;
				d2h.WidthInBytes = pic.w;
				d2h.Height = pic.h + (pic.h >> 1);

				int ret = 0;

				cuvidCtxLock(cuCtxLock, 0);
				/**
				 * Description: copy to host buffer safety
				 */
				if (ret = cuMemcpy2D(&d2h))
				{
					FORMAT_WARNING("copy to host failed", ret);
				}
				cuvidCtxUnlock(cuCtxLock, 0);
			}

			return 0;
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

			if (pic.host_frame)
			{
				framepool.Free(pic.host_frame);
			}

			memset(&pic, 0, sizeof(pic));

			return true;
		}

		inline int Strategy(int s = -1)
		{
			if (s == -1) return qstrategy;
			BOOST_ASSERT((s >= QSWait) && (s < QSMax));
			return (qstrategy = s);
		}
		
		/**
		 * Description: global callbacks
		 */
		static int CUDAAPI HandleVideoSequenceProc(void *p, CUVIDEOFORMAT *pVideoFormat);
		static int CUDAAPI HandlePictureDecodeProc(void *p, CUVIDPICPARAMS *pPicParams);
		static int CUDAAPI HandlePictureDisplayProc(void *p, CUVIDPARSERDISPINFO *pDispInfo);

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
			videoDecodeCreateInfo.ulNumOutputSurfaces	= (qlen);

			/* using dedicated video engines */
			videoDecodeCreateInfo.ulCreationFlags		= cudaVideoCreate_PreferCUVID;

			/* inner decoding cache buffer */
			videoDecodeCreateInfo.ulNumDecodeSurfaces	= (qlen<<1);

			/* context lock */
			videoDecodeCreateInfo.vidLock				= cuCtxLock;

			/* ulNumOutputSurfaces and ulNumDecodeSurfaces is the major param which will affect
			VRAM usage, ulNumOutputSurfaces gives the number of frames can map concurrently in 
			display	callback, ulNumDecodeSurfaces represent nvidia decoder inner buffer upper 
			bound. if decoder is used in a VRAM limited condition, try to adjust the param above */

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
		 * Description: invoked when parsed stream data ready.
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
			while (ret = cuvidMapVideoFrame(cuDecoder, pDispInfo->picture_index, &pSrc,
				&nPitch, &videoProcessingParameters))
			{

			}

			do
			{
				/**
				 * Description: ensure there's room for new picture
				 */

				{
					boost::mutex::scoped_lock(qmtx);
					if (qpic.size() < qlen)
						break;	/* have free space */
					else if (qpic.size() == qlen)
					{
						/* queue full */
						if (QSPopEarliest == qstrategy)
						{
							qpic.pop_front();
							break;
						}
						else if (QSPopLatest == qstrategy)
						{
							/* unmap current frame */
							cuvidUnmapVideoFrame(cuDecoder, pSrc);
							return NV_OK;
						}

					}
				}

				boost::this_thread::sleep(boost::posix_time::milliseconds(20));

			} while (1);

			boost::mutex::scoped_lock(qmtx);
			qpic.push_back(CuFrame(cWidth, cHeight, nPitch, pSrc, pDispInfo->timestamp));

			return ret ? NV_FAILED : NV_OK;
		}

	private:
		/**
		 * Description: cuda objects
		 */
		CUcontext		cuCtx;		/* context handle */
		CUvideoctxlock	cuCtxLock;	/* context lock */
		CUvideoparser	cuParser;	/* video parser handle */
		CUvideodecoder	cuDecoder;	/* video decoder handle */

		/**
		 * Description: current video resolution
		 */
		unsigned int	cWidth;
		unsigned int	cHeight;

		/**
		 * Description: decoded frames list
		 */
		boost::mutex				qmtx;
		unsigned int				qlen;		/* cached for decoded queue length */
		std::list<CuFrame>			qpic;		/* cached for decoded nv12 data */
		boost::atomic_int32_t		qstrategy;	/* queue control strategy */

		enum QueueStrategy
		{
			QSWait			= 0,	/* wait until queue has empty place */
			QSPopEarliest	= 1,	/* pop the earliest one */
			QSPopLatest		= 2,	/* pop the latest one */
			QSMax
		};

		/**
		 * Description: host memory management 
		 */
		bool		bMap2Host;
		HostPool	framepool;	/* RAM pool for frames */
	};

	int CUDAAPI NvDecoder::HandleVideoSequenceProc(void *p, CUVIDEOFORMAT *pVideoFormat)
	{return ((NvDecoder*)p)->HandleVideoSequence(pVideoFormat);}
	int CUDAAPI NvDecoder::HandlePictureDecodeProc(void *p, CUVIDPICPARAMS *pPicParams)
	{return ((NvDecoder*)p)->HandlePictureDecode(pPicParams);}
	int CUDAAPI NvDecoder::HandlePictureDisplayProc(void *p, CUVIDPARSERDISPINFO *pDispInfo)
	{return ((NvDecoder*)p)->HandlePictureDisplay(pDispInfo);}

	/**
	 * Description: media source callback definition
	 */
	typedef void(*MediaSrcDataCallback)(unsigned char *data, unsigned int len, void *p);

	/**
	 * Description: definition of video source parsing
	 */
	class NvMediaSource
	{
	public:
		typedef void(*MediaSrcDataCallback)(unsigned char *data, unsigned int len, void *p);

		NvMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user)
			: eomf(false), datacb(msdcb), cbpointer(user), decoder(NULL)
		{
			BOOST_ASSERT(msdcb);

			reader = new boost::thread(boost::bind(&NvMediaSource::MediaReader, this, srcvideo));
		}

		NvMediaSource(std::string srcvideo, NvDecoder *dec)
			: decoder(dec), datacb(NULL), cbpointer(NULL)
		{
			BOOST_ASSERT(dec);

			reader = new boost::thread(boost::bind(&NvMediaSource::MediaReader, this, srcvideo));
		}

		~NvMediaSource()
		{
			FORMAT_DEBUG(__FUNCTION__, __LINE__, "video source destroyed");
			eomf = true;

			if (reader && reader->joinable())
			{
				reader->join();
			}
		}

		inline bool Eof()
		{
			/* set or get */
			return eomf.load();
		}

		virtual void MediaReader(std::string &filename)
		{
			/**
			 * Description: raw h264 file media source reader implementation
			 */
			FILE *			p = NULL;
			unsigned char * cachedata;	/* stream data cache buffer */
			static const unsigned int cachelen = 1024;
			cachedata = new unsigned char[cachelen];

			BOOST_ASSERT(cachedata);

			if (p = fopen(filename.c_str(), "rb"))
			{
				do
				{
					unsigned int readed = fread(cachedata, 1, cachelen, p);

					if (datacb)
					{
						/**
						* Description: callback to user
						*/
						datacb(cachedata, readed, cbpointer);
						FORMAT_DEBUG(__FUNCTION__, __LINE__, "in callback routine");
					}

					if (decoder)
					{
						/**
						* Description: input to decoder
						*/
						decoder->InputStream(cachedata, readed);
					}

				} while (!(feof(p)||eomf));

				/**
				 * Description: notify end of file
				 */
				if (datacb)		datacb(NULL, 0, cbpointer);
				if (decoder)	decoder->InputStream(NULL, 0);

				std::cout << "end of source file" << std::endl;
			}
			else
			{
				std::cout << "read source file failed" << std::endl;
			}

			if (cachedata)
			{
				delete cachedata;
				cachedata = NULL;
			}

			eomf = true;
		}

	private:
		boost::thread *						reader;		/* stream file reading thread handle */

	protected:
		boost::atomic_bool					eomf;		/* end of media flag */
		NvDecoder *							decoder;	/* NvDecoder object */
		void *								cbpointer;	/* user callback variable */
		NvMediaSource::MediaSrcDataCallback	datacb;		/* user callback */
	};
}
