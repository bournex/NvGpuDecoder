#pragma once
#include <iostream>
#include <string>
#include <list>
#include <map>
#include <boost/thread.hpp>
#include <boost/bind.hpp>

extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
}

#include "DedicatedPool.h"
#include "BaseCodec.h"

using namespace std;

#define ALIGNED_SIZE	4
#define AVFRAME2CUFRAME(avf, cuf)	\
	cuf.host_frame	= (unsigned char*)avf->data;\
	cuf.w			= avf->width;\
	cuf.h			= avf->height;\
	cuf.timestamp	= avf->best_effort_timestamp;

namespace FFCodec
{
	bool FFInit()
	{
		av_register_all();
		avformat_network_init();

		return true;
	};

	class FFMpegCodec : public BaseCodec
	{
	public:
		class FFCodecPool
		{
		public:
			AVFrame *Alloc(int width, int height)
			{
				AVFrame *buf = NULL;
				do
				{
					boost::lock_guard<boost::mutex> lk(mtx);

					/**
					* Description: find proper buffer in freelist
					*/
					for (std::list<AVFrame*>::iterator it = freelist.begin();
						it != freelist.end();
						it++)
					{
						/**
						* Description: find buffer in freelist
						*/
						if (((*it)->width * (*it)->height) >= (width * height))
						{
							buf = *it;
							busylist.insert(std::pair<void*, AVFrame*>(buf->data, buf));
							freelist.erase(it);

							break;
						}
					}

					if (!buf)
					{
						if ((freelist.size() + busylist.size()) < 1024)
						{
							/**
							* Description: no proper size buffer, alloc heap memory
							*/
							if (!buf)
							{
								buf = av_frame_alloc();

								av_image_fill_arrays(buf->data, buf->linesize,
									(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, ALIGNED_SIZE)),
									AV_PIX_FMT_NV12, width, height, ALIGNED_SIZE);

								busylist.insert(std::pair<void*, AVFrame*>(buf->data, buf));
							}
						}
						else if (freelist.size())
						{
							/**
							* Description: no suitable free buffer, realloc the biggest one
							*/
							std::list<AVFrame*>::reverse_iterator it = freelist.rbegin();
							buf = (*it);

							/**
							* Description: realloc
							*/
							av_frame_free(&buf);
							buf = av_frame_alloc();

							av_image_fill_arrays(buf->data, buf->linesize,
								(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, ALIGNED_SIZE)),
								AV_PIX_FMT_NV12, width, height, ALIGNED_SIZE);

							BOOST_ASSERT(buf);

							/**
							* Description: enqueue worklist, dequeue freelist
							*/
							busylist.insert(std::pair<void*, AVFrame*>(buf->data, buf));
							freelist.erase(it.base());
						}
						else
						{
							/**
							* Description: worklist full, wait for next around, [TODO liuxf] reduce cpu usage
							*/
						}
					}

					/**
					* Description: try until get suitable buffer
					*/

				} while (!buf);

				return buf;
			}

			bool Free(void* frame)
			{
				boost::lock_guard<boost::mutex> lk(mtx);

				std::map<void*, AVFrame*>::iterator it = busylist.find(frame);
				if (it == busylist.end())
				{
					FORMAT_WARNING("buffer unrecognized", 0);
					return false;
				}
				else
				{
					/**
					* Description: return buffer to freelist
					*/

					freelist.push_back(it->second);
					busylist.erase(it);
				}

				return true;
			}

		private:
			boost::mutex			mtx;
			list<AVFrame*>			freelist;
			map<void*, AVFrame*>	busylist;
		};

		FFMpegCodec() : cuCtx(NULL), cuCtxLock(NULL)
		{
			pCodecCtx = avcodec_alloc_context3(NULL);
			if (pCodecCtx == NULL)
			{
				printf("Could not allocate AVCodecContext\n");
				throw (-1);
			}

			int ret = 0;
			if (ret = cuInit(0))
			{
				FORMAT_FATAL("create init environment failed", ret);
				throw ret;
			}

			if (!cuCtx)
			{
				if (ret = cuCtxCreate(&cuCtx, 0, 0))
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
		}

		~FFMpegCodec()
		{
			/**
			* Description: free scale object
			*/
			sws_freeContext(img_convert_ctx);

			/**
			* Description: free decode frame pointer
			*/
			av_frame_free(&pFrame);

			/**
			* Description: free context
			*/
			avcodec_close(pCodecCtx);
		}

		void Create(AVCodecParameters *pCodecPara)
		{
			/**
			* Description: set codec parameter
			*/
			avcodec_parameters_to_context(pCodecCtx, pCodecPara);

			/**
			* Description: find decoder by id
			*/
			pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
			if (pCodec == NULL) {
				printf("Codec not found.\n");
				throw (-1);
			}

			/**
			* Description: create decoder
			*/
			if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
				printf("Could not open codec.\n");
				throw (-1);
			}

			/**
			* Description: alloc decode frame pointer
			*/
			pFrame = av_frame_alloc();

			/**
			* Description: create scale object
			*/
			img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
				pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_NV12, SWS_BICUBIC, NULL, NULL, NULL);
		}

		virtual bool InputStream(unsigned char* pStream, unsigned int nSize)
		{
			BOOST_ASSERT(pStream);
			BOOST_ASSERT(nSize);

			AVPacket *packet = (AVPacket*)pStream;

			ret = avcodec_send_packet(pCodecCtx, packet);
			if (ret < 0) {
				printf("Decode Error.\n");
				return false;
			}

			/**
			 * Description: alloc NV12 scaled buffer
			 */
			AVFrame* avf = avfpool.Alloc(pFrame->width, pFrame->height);
			if (AVERROR(EAGAIN) == avcodec_receive_frame(pCodecCtx, avf))
			{
				avfpool.Free(avf->data);
				return false;
			}

			sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
				avf->data, avf->linesize);

			boost::lock_guard<boost::mutex> lk(mtx);
			avfq.push_back(avf);

			return true;
		}


		virtual int GetFrame(NvCodec::CuFrame &pic)
		{
			boost::lock_guard<boost::mutex> lk(mtx);

			if (avfq.size())
			{
				AVFrame *avf = avfq.front();
				AVFRAME2CUFRAME(avf, pic);

				static int index = 0;
				char szName[512] = { 0 };
				sprintf(szName, "%d.yuv", index++);
				FILE *pf = fopen(szName, "wb");
				fwrite(avf->data[0], 1, (avf->width * avf->height), pf);
				fwrite(avf->data[1], 1, (avf->width * avf->height)>>2, pf);
				fwrite(avf->data[2], 1, (avf->width * avf->height)>>2, pf);
				fclose(pf);

				pic.dev_frame = (CUdeviceptr)devpool.Alloc(GPU_NV12_CALC(avf->width, avf->height));
				pic.dev_pitch = GPU_WIDTH_ALIGN(avf->width);

				//CUDA_MEMCPY2D d2h = { 0 };
				//d2h.srcMemoryType = CU_MEMORYTYPE_HOST;
				//d2h.dstMemoryType = CU_MEMORYTYPE_DEVICE;
				//d2h.srcHost = pic.host_frame;
				//// d2h.dstHost = (void*)pic.dev_frame;
				//d2h.dstDevice = (CUdeviceptr)pic.host_frame;
				//d2h.srcPitch = pic.w;
				//d2h.dstPitch = pic.dev_pitch;
				//d2h.WidthInBytes = pic.w;
				//d2h.Height = pic.h + (pic.h >> 1);

				int ret = 0;

				//cuvidCtxLock(cuCtxLock, 0);
				///**
				//* Description: copy to host buffer safety
				//*/
				//if (ret = cuMemcpy2D(&d2h))
				//{
				//	FORMAT_WARNING("copy to host failed", ret);
				//}
				//cuvidCtxUnlock(cuCtxLock, 0);

				avfq.pop_front();
				return 0;
			}

			return -1;
		}

		virtual bool PutFrame(NvCodec::CuFrame &pic)
		{
			BOOST_ASSERT(pic.host_frame);

			boost::lock_guard<boost::mutex> lk(mtx);
			return avfpool.Free(pic.host_frame);
		}

	private:
		bool				bplaying;
		void *				usr;

		boost::mutex		mtx;
		FFCodecPool			avfpool;
		list<AVFrame*>		avfq;
		CUcontext			cuCtx;		/* context handle */
		CUvideoctxlock		cuCtxLock;	/* context lock */

		AVCodecContext*		pCodecCtx;
		AVCodec*			pCodec;
		AVFrame*			pFrame;
		AVPacket *			packet;
		int					ret;
		int					got_picture;
		struct SwsContext *	img_convert_ctx;
		DevicePool			devpool;
	};

	class FFMediaSource : public BaseMediaSource
	{
	public:

		FFMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user, unsigned int cachesize = 1024)
			: BaseMediaSource(srcvideo, msdcb, user)
		{
			pFormatCtx = avformat_alloc_context();

			if (avformat_open_input(&pFormatCtx, srcvideo.c_str(), NULL, NULL) != 0) {
				printf("Couldn't open input stream.\n");
				throw (-1);
			}

			if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
				printf("Couldn't find stream information.\n");
				throw (-1);
			}
			videoindex = -1;
			for (int i = 0; i < pFormatCtx->nb_streams; i++)
				if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					videoindex = i;
					break;
				}

			if (videoindex == -1) {
				printf("Didn't find a video stream.\n");
				throw (-1);
			}

			packet = (AVPacket*)av_packet_alloc();

			if (!hworker)
			{
				bplaying = true;
				hworker = new boost::thread(boost::bind(&FFMediaSource::MediaReader, this, src));

				BOOST_ASSERT(hworker);
			}
		}

		FFMediaSource(std::string srcvideo, BaseCodec *dec, unsigned int cachesize = 1024)
			: BaseMediaSource(srcvideo, dec), hworker(NULL), packet(NULL), pFormatCtx(NULL), bplaying(false)
		{
			int ret = 0;

			pFormatCtx = avformat_alloc_context();

			if ((ret = avformat_open_input(&pFormatCtx, srcvideo.c_str(), NULL, NULL)) != 0) {
				printf("Couldn't open input stream.\n");
				throw (-1);
			}

			if ((ret = avformat_find_stream_info(pFormatCtx, NULL)) < 0) {
				printf("Couldn't find stream information.\n");
				throw (-1);
			}

			videoindex = -1;
			for (int i = 0; i < pFormatCtx->nb_streams; i++)
				if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
					((FFMpegCodec*)dec)->Create(pFormatCtx->streams[i]->codecpar);
					videoindex = i;
					break;
				}

			if (videoindex == -1) {
				printf("Didn't find a video stream.\n");
				throw (-1);
			}

			packet = (AVPacket*)av_packet_alloc();
			BOOST_ASSERT(packet);

			if (!hworker)
			{
				bplaying = true;
				hworker = new boost::thread(boost::bind(&FFMediaSource::MediaReader, this, src));

				BOOST_ASSERT(hworker);
			}
		}

		virtual ~FFMediaSource()
		{
			bplaying = false;
			if (hworker && hworker->joinable())
			{
				hworker->join();
				delete hworker;
				hworker = NULL;
			}

			if (packet)
			{
				av_packet_unref(packet);
				packet = NULL;
			}

			if (pFormatCtx)
			{
				avformat_close_input(&pFormatCtx);
				pFormatCtx = NULL;
			}
		}

		void MediaReader(std::string &filename)
		{
			while (av_read_frame(pFormatCtx, packet) >= 0 && bplaying/* exit flag */)
			{
				if (packet->stream_index == videoindex) {

					if (datacb)
					{
						/**
						* Description: callback to user
						*/
						datacb((unsigned char *)packet, sizeof(AVPacket*), cbpointer);
						FORMAT_DEBUG(__FUNCTION__, __LINE__, "in callback routine");
					}

					if (decoder)
					{
						/**
						* Description: input to decoder
						*/
						decoder->InputStream((unsigned char *)packet, sizeof(AVPacket*));
					}
				}
			}
		}

	private:

		int					videoindex;
		AVPacket*			packet;			/* video media data package */
		AVFormatContext*	pFormatCtx;		/* video format context */
		bool				bplaying;		/* playing flag */
		boost::thread *		hworker;		/* playing thread handle */
	};

}
