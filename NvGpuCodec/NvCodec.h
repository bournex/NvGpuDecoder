#pragma once


#include <boost/thread/lock_guard.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread.hpp>
#include <boost/atomic.hpp>
#include "cuda_runtime_api.h"
#include <nvcuvid.h>
#include <string>
#include <list>
#include <chrono>
using namespace std::chrono;

#include "BaseCodec.h"
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
	int NvCodecInit(int devno, CUcontext &cudactx)
	{
		int ret = 0;

		if (ret = cuInit(0))
		{
			FORMAT_FATAL("create init environment failed", ret);
			return ret;
		}

		if (ret = cuCtxCreate(&cudactx, 0, devno))
		{
			FORMAT_FATAL("create context failed", ret);
			return ret;
		}

		return 0;
	}

	int NvCodecUninit(CUcontext cudactx)
	{
		if (cudactx)
		{
			/**
			 * Description: if outer created context , do not destroy
			 */
		}

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

	class NvDecoder : public BaseCodec
	{
	public:
		/**
		* Description: 
		*/
		NvDecoder(unsigned int devidx = 0, unsigned int queuelen = 8, void *cudactx = NULL, DevicePool* devpool = NULL, bool map2host = false)
			: cuCtx((CUcontext)cudactx)
			, cuCtxLock(NULL)
			, cuParser(NULL)
			, cuDecoder(NULL)
			, dev(devidx)
			, beof(FALSE)
			, cWidth(0)
			, cHeight(0)
			, qlen((queuelen*3)>>1)
			, bMap2Host(map2host)
			, bLocalPool((devpool == NULL) ? true : false)
			, devicepool(devpool)
			, framepool((queuelen<<2))
		{
			int ret = Init();
			if (ret) throw ret;
		}

		int Init()
		{
			int ret = 0;

			if (!devicepool)
			{
				devicepool = new DevicePool(qlen);
			}

			ctxcreatelock.lock();
			CUcontext ctxOld = NULL;
			cuCtxPopCurrent(&ctxOld);

			if (!ctxOld)
			{
				if (!cuCtx)
				{
					if (ret = cuCtxCreate(&cuCtx, 0, dev))
					{
						FORMAT_FATAL("create context failed", ret);
						return ret;
					}
				}

				if ((ret = cuCtxPushCurrent(cuCtx)) != 0)
				{
					FORMAT_FATAL("push context failed", ret);
					return ret;
				}
			}

			if (!cuCtxLock && (ret = cuvidCtxLockCreate(&cuCtxLock, cuCtx)))
			{
				FORMAT_FATAL("create context lock failed", ret);
				ctxcreatelock.unlock();
				return ret;
			}
			ctxcreatelock.unlock();

			/**
			* Description: create video parser
			*/
			CUVIDPARSERPARAMS videoParserParameters = {};
			/* my sample only support h264 stream */
			videoParserParameters.CodecType = cudaVideoCodec_H264;
			/* stream cached length */
			videoParserParameters.ulMaxNumDecodeSurfaces = (qlen << 1);
			/* delay for 1 */
			videoParserParameters.ulMaxDisplayDelay = 1;
			/* user data */
			videoParserParameters.pUserData = this;
			/* callbacks */
			videoParserParameters.pfnSequenceCallback = HandleVideoSequenceProc;
			videoParserParameters.pfnDecodePicture = HandlePictureDecodeProc;
			videoParserParameters.pfnDisplayPicture = HandlePictureDisplayProc;

			if (ret = cuvidCreateVideoParser(&cuParser, &videoParserParameters))
			{
				FORMAT_FATAL("create video parser failed", ret);
				return ret;
			}

			return ret;
		}

		/**
		* Description: 
		*/
		virtual ~NvDecoder()
		{
			int ret = 0;

			boost::lock_guard<boost::recursive_mutex> lock(qmtx);
			for (std::list<CuFrame>::iterator it = qpic.begin(); it != qpic.end(); it++)
			{
#ifdef WIN32
				if (ret = cuvidUnmapVideoFrame(cuDecoder, (unsigned int)it->dev_frame))
#else
				if (ret = cuvidUnmapVideoFrame(cuDecoder, (unsigned long long)it->dev_frame))
#endif
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

			if (bLocalPool)
			{
				delete devicepool;
				devicepool = NULL;
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
				{
					packet.flags	= CUVID_PKT_ENDOFSTREAM;
					beof			= true;
				}

				// std::cout<<"address "<<std::setbase(16)<<pStream<<", datalen "<<nSize<<std::endl;
				if (ret = cuvidParseVideoData(cuParser, &packet))
				{
					FORMAT_FATAL("parse video data failed", ret);
				}
				else
				{
					if (beof)
					{
						CuFrame & lastframe = qpic.back();
						lastframe.last = true;
						beof = false;
					}

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
				boost::lock_guard<boost::recursive_mutex> lk(qmtx);
				if (qpic.size() && !beof)
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

				int ret = cudaMemcpy(pic.host_frame, pic.dev_frame, pic.dev_pitch * pic.h + ((pic.dev_pitch * pic.h) >> 1), cudaMemcpyDeviceToHost);
				if (ret)
				{
					FORMAT_FATAL("copy frame from device to host failed", ret);
					return -1;
				}
			}

			return 0;
		}

		inline bool PutFrame(CuFrame &pic)
		{
			int ret = 0;

			// boost::lock_guard<boost::recursive_mutex> lk(qmtx);

			if (pic.dev_frame)
				devicepool->Free((unsigned char *)pic.dev_frame);

			if (pic.host_frame)
				framepool.Free(pic.host_frame);

			memset(&pic, 0, sizeof(pic));

			return true;
		}

		inline int Strategy(int s = -1)
		{
			/* get */
			if (s == -1) return qstrategy;
			BOOST_ASSERT((s >= QSWait) && (s < QSMax));
			/* set */
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

			CUcontext ctxOld;
			cuCtxPopCurrent(&ctxOld);

			if (!ctxOld)
			{
				if (!cuCtx)
				{
					if (ret = cuCtxCreate(&cuCtx, 0, dev))
					{
						FORMAT_FATAL("create context failed", ret);
						return ret;
					}
				}

				if ((ret = cuCtxPushCurrent(cuCtx)) != 0)
				{
					FORMAT_FATAL("push context failed", ret);
					return ret;
				}
			}

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
			videoDecodeCreateInfo.ulNumOutputSurfaces	= (1);

			/* using dedicated video engines */
			videoDecodeCreateInfo.ulCreationFlags		= cudaVideoCreate_PreferCUVID;

			/* inner decoding cache buffer */
			videoDecodeCreateInfo.ulNumDecodeSurfaces	= (12);

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

			/**
			 * Description: reset epoch
			 */
			epoch = system_clock::now();

			return ret ? NV_FAILED : NV_OK;
		}

		/**
		 * Description: invoked when parsed stream data ready.
		 */
		int HandlePictureDecode(CUVIDPICPARAMS *pPicParams)
		{
			if (!cuDecoder)
			{
				FORMAT_WARNING("video decoder not created yet", -1);
				return NV_FAILED;
			}

			cuvidCtxLock(cuCtxLock, 0);
			int ret = cuvidDecodePicture(cuDecoder, pPicParams);
			cuvidCtxUnlock(cuCtxLock, 0);

			if (ret)
			{
				FORMAT_FATAL("decode picture failed", ret);
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
				boost::this_thread::sleep(boost::posix_time::microseconds(100));
			}

			// return cuvidUnmapVideoFrame(cuDecoder, pSrc);

			do
			{
				/**
				 * Description: ensure there's room for new picture
				 */
				{
					if (qmtx.try_lock())
					{
						static const system_clock::duration dn(1000 * 40);
						int fluc = rand() % 200;
						system_clock::duration dran(((fluc & 0x00000001) ? fluc : -fluc));

						if (beof) qlen++;
						if (qpic.size() < (qlen))
						{
							/* have free space */
							void* devbuf = devicepool->Alloc((nPitch * cHeight * 3) >> 1);
							BOOST_ASSERT(devbuf);

							cuvidCtxLock(cuCtxLock, 0);
							ret = cudaMemcpy((void*)devbuf, (void*)pSrc, (nPitch * cHeight * 3) >> 1, cudaMemcpyDeviceToDevice);
							cuvidCtxUnlock(cuCtxLock, 0);

							qpic.push_back(CuFrame(cWidth, cHeight, nPitch, devbuf, (epoch + dn + dran).time_since_epoch().count()));
							epoch += (dn + dran);
							qmtx.unlock();
							break;
						}
						else if (qpic.size() == qlen)
						{
							/* queue full */
							if (QSPopEarliest == qstrategy)
							{
								void* devbuf = devicepool->Alloc((nPitch*cHeight * 3) >> 1);
								BOOST_ASSERT(devbuf);

								ret = cudaMemcpy((void*)devbuf, (void*)pSrc, (nPitch * cHeight * 3) >> 1, cudaMemcpyDeviceToDevice);
								if (ret)
								{
									FORMAT_FATAL("copy decoded frame failed", ret);
								}

								qpic.pop_front();
								qpic.push_back(CuFrame(cWidth, cHeight, nPitch, devbuf, pDispInfo->timestamp));
								qmtx.unlock();
								break;
							}
							else if (QSPopLatest == qstrategy)
							{
								/* unmap current frame without queueing */
								qmtx.unlock();
								break;
							}
							qmtx.unlock();
						}
					}
				}

				boost::this_thread::sleep(boost::posix_time::microseconds(1500));

			} while (1);


			return cuvidUnmapVideoFrame(cuDecoder, pSrc);
		}

	private:
		/**
		 * Description: cuda objects
		 */
		int				dev;
		static boost::mutex ctxcreatelock;		/* context handle */
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
		boost::recursive_mutex		qmtx;
		unsigned int				qlen;		/* cached for decoded queue length */
		std::list<CuFrame>			qpic;		/* cached for decoded nv12 data */
		boost::atomic_bool			beof;		/* end of video frame */

		/**
		 * Description: host memory management 
		 */
		bool		bMap2Host;
		HostPool	framepool;		/* RAM pool for frames */
		bool		bLocalPool;
		DevicePool	*devicepool;	/* VRAM pool for frames */
		system_clock::time_point epoch;
	};
	boost::mutex NvDecoder::ctxcreatelock;
	// CUcontext __declspec(thread) NvDecoder::cuCtx = 0;

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
	class NvMediaSource : public BaseMediaSource
	{
	public:
		typedef void(*MediaSrcDataCallback)(unsigned char *data, unsigned int len, void *p);

		NvMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user)
			: reader(NULL), eomf(false), BaseMediaSource(srcvideo, msdcb, user)
		{
			BOOST_ASSERT(msdcb);

			reader = new boost::thread(boost::bind(&NvMediaSource::MediaReader, this, srcvideo));

			BOOST_ASSERT(reader);
		}

		NvMediaSource(std::string srcvideo, BaseCodec *dec)
			: reader(NULL), eomf(false), BaseMediaSource(srcvideo, dec)
		{
			reader = new boost::thread(boost::bind(&NvMediaSource::MediaReader, this, srcvideo));

			BOOST_ASSERT(reader);
		}

		virtual ~NvMediaSource()
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

	protected:
		boost::atomic_bool	eomf;		/* end of media flag */
		boost::thread *		reader;		/* stream file reading thread handle */
	};
}
