// GpuCodec.cpp : �������̨Ӧ�ó������ڵ㡣
//

#include "stdafx.h"
#include "NvCodec.h"
#include "MTGpuFramework.h"
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

			decoder.PutFrame(frame);
		}
	}

	return 0;
}