// GpuCodec.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "NvCodec.h"
#include <boost/filesystem.hpp>
#include <iostream>

using namespace std;

int main(int argc, char **argv)
{
	if (!argv[1] || !boost::filesystem::exists(boost::filesystem::path(argv[1])))
	{
		cout<<"invalid h264 file parameter ...."<<endl<<"GpuCodec.exe path_file_to_decode.h264"<<endl;
		return 0;
	}

	/**
	 * Description: create media source & decoder
	 */
	NvCodec::NvDecoder decoder;
	NvCodec::NvDecoder::CuFrame frame;
	NvCodec::NvMediaSource media(argv[1], NULL, NULL, &decoder);

	// FILE *nv12 = fopen("D:\\out.yuv", "wb");

	while (!media.Eof())
	{
		if (!decoder.GetFrame(frame))
		{
			Sleep(1);
		}
		else
		{
			/**
			 * Description: process the nv12 frame
			 */
			//if (frame.host_frame)
			//	fwrite(frame.host_frame, 1, frame.w * frame.h, nv12);

			decoder.PutFrame(frame);
		}
	}

	//?fclose(nv12);
	//nv12 = NULL;

	return 0;
}