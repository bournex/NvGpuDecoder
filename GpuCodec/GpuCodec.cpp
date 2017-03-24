// GpuCodec.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "NvCodec.h"
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/bind.hpp>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
using namespace std;

class H264Decoder : public NvCodec::NvDecoder
{
public:
	H264Decoder():NvCodec::NvDecoder(){}
	void Input(unsigned char* buf, unsigned int len)
	{
		InputStream(buf, len);
	}
};

class StreamSource
{
private:
	bool				bfinish;
	fstream				videosrc;
	boost::thread *		feeder;
	H264Decoder &		decoder;
	FILE *				p;
public:
	StreamSource(string filename, H264Decoder &_decoder):bfinish(false), decoder(_decoder)
	{
		feeder = new boost::thread(boost::bind(&StreamSource::threadworker, this, filename));
	}

	~StreamSource()
	{
		bfinish = true;
		fclose(p);
		p = NULL;
	}


	inline bool ReachEnd(){return (feof(p) != 0);}

	void threadworker(string filename)
	{
#if 0
		videosrc.open(filename.c_str());

		if (!videosrc.is_open())
		{
			cout<<"open file '"<<filename<<"' failed"<<endl;
			return;
		}

		unsigned char streamdata[1024] = {0};

		while (!bfinish && !videosrc.eof())
		{
			videosrc.read((char*)streamdata, 1024);
			decoder.Input(streamdata, 1024);
		}

		if (videosrc.eof())
		{
			cout<<"finish read"<<endl;
		}
#else
		p = fopen(filename.c_str(), "rb");

		if (p)
		{
			unsigned char streamdata[1024] = {0};

			do
			{
				unsigned int readed = fread(streamdata, 1, 1024, p);
				decoder.Input(streamdata, readed);

			}while(!feof(p));

			cout<<"end of source file"<<endl;

		}
#endif
	}
};

int main(int argc, char **argv)
{
	if (!argv[1] || !boost::filesystem::exists(boost::filesystem::path(argv[1])))
	{
		cout<<"invalid h264 file parameter ...."<<endl<<"GpuCodec.exe path_file_to_decode.h264"<<endl;
		return 0;
	}

	H264Decoder decoder;
	StreamSource ss(argv[1], decoder);

	NvCodec::NvDecoder::CuFrame frame;

	while (!ss.ReachEnd())
	{
		if (decoder.GetFrame(frame))
		{
			Sleep(5);
			decoder.PutFrame(frame);
		}
	}

	return 0;
}