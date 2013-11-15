
#ifndef BASE_H
#define BASE_H

#include <string>

class FrameMetaData
{
public:
	std::string fmt;
	int width;
	int height;
	unsigned buffLen;
	unsigned long sequence;
	unsigned long tv_sec;
	unsigned long tv_usec;

	FrameMetaData()
	{
		width = 0;
		height = 0;
		buffLen = 0;
		sequence = 0;
		tv_sec = 0;
		tv_usec = 0;
	}

	FrameMetaData(const FrameMetaData &in)
	{
		FrameMetaData::operator=(in);
	}	

	const FrameMetaData &operator=(const FrameMetaData &in)
	{
		width = in.width;
		height = in.height;
		fmt = in.fmt;
		buffLen = in.buffLen;
		sequence = in.sequence;
		tv_sec = in.tv_sec;
		tv_usec = in.tv_usec;
		return *this;
	}

};

// **********************************************************************

class Base_Video_In
{
public:
	virtual void Stop() {};
	virtual void WaitForStop() {};
	virtual void OpenDevice() {};
	virtual void SetFormat(const char *fmt, int width, int height) {};
	virtual void StartDevice(int buffer_count) {};
	virtual void StopDevice() {};
	virtual void CloseDevice() {};
	virtual int GetFrame(unsigned char **buffOut, class FrameMetaData *metaOut) {return 0;};
};

#endif //BASE_H
