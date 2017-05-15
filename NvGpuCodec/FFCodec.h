#pragma once
#include <iostream>
#include <string>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"

#include "BaseCodec.h"

class FFCodec : public BaseCodec
{
public:
	FFCodec()
	{
		pCodecCtx = avcodec_alloc_context3(NULL);
		if (pCodecCtx == NULL)
		{
			printf("Could not allocate AVCodecContext\n");
			throw (-1);
		}

	}

	~FFCodec()
	{
		while (1 && bplaying) {
			ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
			if (ret < 0)
				break;
			if (!got_picture)
				break;
			sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
				pFrameYUV->data, pFrameYUV->linesize);

			int y_size = pCodecCtx->width*pCodecCtx->height;
		}

		sws_freeContext(img_convert_ctx);

		av_frame_free(&pFrameYUV);
		av_frame_free(&pFrame);
		avcodec_close(pCodecCtx);
	}

	void Create(AVCodecParameters *parameters)
	{
		avcodec_parameters_to_context(pCodecCtx, parameters);

		pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
		if (pCodec == NULL) {
			printf("Codec not found.\n");
			throw (-1);
		}
		if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
			printf("Could not open codec.\n");
			throw (-1);
		}

		pFrame = av_frame_alloc();
		pFrameYUV = av_frame_alloc();
		out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1));
		av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer,
			AV_PIX_FMT_YUV420P, pCodecCtx->width, pCodecCtx->height, 1);

		img_convert_ctx = sws_getContext(pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
			pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);
	}

	virtual bool InputStream(unsigned char* pStream, unsigned int nSize)
	{
		BOOST_ASSERT(pStream);
		BOOST_ASSERT(nSize);

		AVPacket *packet = (AVPacket*)pStream;

		ret = avcodec_decode_video2(pCodecCtx, pFrame, &got_picture, packet);
		if (ret < 0){
			printf("Decode Error.\n");
			return -1;
		}
		if (got_picture){
			sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, pCodecCtx->height,
				pFrameYUV->data, pFrameYUV->linesize);

			y_size = pCodecCtx->width*pCodecCtx->height;
		}
	}

	virtual int GetFrame(NvCodec::CuFrame &pic)
	{

	}

	virtual bool PutFrame(NvCodec::CuFrame &pic)
	{

	}

private:
	bool				bplaying;
	void *				usr;

	AVCodecContext*		pCodecCtx;
	AVCodecParameters*	pCodecPara;
	AVCodec*			pCodec;
	AVFrame*			pFrame;
	AVFrame*			pFrameYUV;
	unsigned char *		out_buffer;
	AVPacket *			packet;
	int					y_size;
	int					ret;
	int					got_picture;
	struct SwsContext *	img_convert_ctx;
};