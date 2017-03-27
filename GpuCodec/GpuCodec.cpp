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

	NvCodec::NvDecoder decoder;
	NvCodec::NvDecoder::CuFrame frame;
	NvCodec::NvMediaSource ms(argv[1], &decoder);

	while (!ms.Eof() || decoder.GetFrame(frame))
	{
		Sleep(5);
		/**
		 * Description: process the nv12 frame
		 */

		decoder.PutFrame(frame);
	}

	return 0;
}