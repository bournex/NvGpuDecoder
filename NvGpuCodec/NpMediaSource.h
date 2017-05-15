#pragma once

#include "FFCodec.h"
#include "NvCodec.h"
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"


namespace NvCodec
{
	class NpMediaSource : public BaseMediaSource
	{
	public:

		NpMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user, unsigned int cachesize = 1024)
			: BaseMediaSource(srcvideo, msdcb, user)
		{
		}

		NpMediaSource(std::string srcvideo, NvDecoder *dec, unsigned int cachesize = 1024)
			: BaseMediaSource(srcvideo, dec)
		{
		}

		virtual ~NpMediaSource()
		{
		}
	};

	class FFMediaSource : public BaseMediaSource
	{
	public:

		FFMediaSource(std::string srcvideo, MediaSrcDataCallback msdcb, void*user, unsigned int cachesize = 1024)
			: BaseMediaSource(srcvideo, msdcb, user)
		{
			pFormatCtx = avformat_alloc_context();

			if (avformat_open_input(&pFormatCtx, srcvideo.c_str(), NULL, NULL) != 0){
				printf("Couldn't open input stream.\n");
				throw (-1);
			}

			if (avformat_find_stream_info(pFormatCtx, NULL) < 0){
				printf("Couldn't find stream information.\n");
				throw (-1);
			}
			videoindex = -1;
			for (int i = 0; i < pFormatCtx->nb_streams; i++)
				if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
					videoindex = i;
					break;
				}

			if (videoindex == -1){
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
			: BaseMediaSource(srcvideo, dec)
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
					((FFCodec*)dec)->Create(pFormatCtx->streams[i]->codecpar);
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
				if (packet->stream_index == videoindex){

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
