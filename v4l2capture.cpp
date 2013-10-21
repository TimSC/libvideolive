// python-v4l2capture
// Python extension to capture video with video4linux2
//
// 2009, 2010, 2011 Fredrik Portstrom, released into the public domain
// 2011, Joakim Gebart
// 2013, Tim Sheerman-Chase
// See README for license

#define USE_LIBV4L

#include <Python.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <string.h>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <pthread.h>

#ifdef USE_LIBV4L
#include <libv4l2.h>
#else
#include <sys/ioctl.h>
#define v4l2_close close
#define v4l2_ioctl ioctl
#define v4l2_mmap mmap
#define v4l2_munmap munmap
#define v4l2_open open
#endif

#define ASSERT_OPEN if(self->fd < 0)					\
		{									\
			PyErr_SetString(PyExc_ValueError,					\
		"I/O operation on closed file");				\
			Py_RETURN_NONE;							\
		}

struct buffer {
	void *start;
	size_t length;
};

typedef struct {
	PyObject_HEAD
	int fd;
	struct buffer *buffers;
	int buffer_count;
} Video_device;

class Device_manager_cl{
public:
	PyObject_HEAD
	std::map<std::string, class Device_manager_Worker_thread_args *> *threadArgStore;
};
typedef Device_manager_cl Device_manager;

static PyObject *Device_manager_stop(Device_manager *self, PyObject *args);
static PyObject *Device_manager_close(Device_manager *self, PyObject *args);

struct capability {
	int id;
	const char *name;
};

static struct capability capabilities[] = {
	{ V4L2_CAP_ASYNCIO, "asyncio" },
	{ V4L2_CAP_AUDIO, "audio" },
	{ V4L2_CAP_HW_FREQ_SEEK, "hw_freq_seek" },
	{ V4L2_CAP_RADIO, "radio" },
	{ V4L2_CAP_RDS_CAPTURE, "rds_capture" },
	{ V4L2_CAP_READWRITE, "readwrite" },
	{ V4L2_CAP_SLICED_VBI_CAPTURE, "sliced_vbi_capture" },
	{ V4L2_CAP_SLICED_VBI_OUTPUT, "sliced_vbi_output" },
	{ V4L2_CAP_STREAMING, "streaming" },
	{ V4L2_CAP_TUNER, "tuner" },
	{ V4L2_CAP_VBI_CAPTURE, "vbi_capture" },
	{ V4L2_CAP_VBI_OUTPUT, "vbi_output" },
	{ V4L2_CAP_VIDEO_CAPTURE, "video_capture" },
	{ V4L2_CAP_VIDEO_OUTPUT, "video_output" },
	{ V4L2_CAP_VIDEO_OUTPUT_OVERLAY, "video_output_overlay" },
	{ V4L2_CAP_VIDEO_OVERLAY, "video_overlay" }
};

static int my_ioctl(int fd, int request, void *arg, int utimeout = -1)
{
	// Retry ioctl until it returns without being interrupted.

	for(;;)
	{
		// Wait for frame until time out
		if(utimeout >= 0)
		{

			fd_set fds;
			FD_ZERO (&fds);
			FD_SET (fd, &fds);

			struct timeval tv;
		    tv.tv_sec = 0;
		    tv.tv_usec = utimeout;
			int r = select(fd+1, &fds, NULL, NULL, &tv);
			
			if(r == 0)
				return 1; //Timed out
		}

		//printf("call\n");
		int result = v4l2_ioctl(fd, request, arg);
		//printf("%d\n", result);

		if(!result)
		{
			//printf("ret\n");
			return 0;
		}

		if(errno == EAGAIN)
		{
			//printf("ret\n");
			return 1;
		}

		if(errno != EINTR)
		{
			PyErr_SetFromErrno(PyExc_IOError);
			return 1;
		}
		usleep(1000);
	}
}

static void Video_device_unmap(Video_device *self)
{
	int i;

	for(i = 0; i < self->buffer_count; i++)
	{
		v4l2_munmap(self->buffers[i].start, self->buffers[i].length);
	}
	free(self->buffers);
	self->buffers = NULL;
}

static void Video_device_dealloc(Video_device *self)
{
	if(self->fd >= 0)
	{
		if(self->buffers)
		{
			Video_device_unmap(self);
		}

		v4l2_close(self->fd);
	}

	self->ob_type->tp_free((PyObject *)self);
}

static int Video_device_init(Video_device *self, PyObject *args,
		PyObject *kwargs)
{
	const char *device_path;

	if(!PyArg_ParseTuple(args, "s", &device_path))
		{
			return -1;
		}

	int fd = v4l2_open(device_path, O_RDWR | O_NONBLOCK);

	if(fd < 0)
		{
			PyErr_SetFromErrnoWithFilename(PyExc_IOError, (char *)device_path);
			return -1;
		}

	self->fd = fd;
	self->buffers = NULL;
	return 0;
}

static PyObject *Video_device_close(Video_device *self)
{
	if(self->fd >= 0)
		{
			if(self->buffers)
	{
		Video_device_unmap(self);
	}

			v4l2_close(self->fd);
			self->fd = -1;
		}

	Py_RETURN_NONE;
}

static PyObject *Video_device_fileno(Video_device *self)
{
	ASSERT_OPEN;
	return PyInt_FromLong(self->fd);
}

static PyObject *Video_device_get_info(Video_device *self)
{
	ASSERT_OPEN;
	struct v4l2_capability caps;

	if(my_ioctl(self->fd, VIDIOC_QUERYCAP, &caps))
	{
		Py_RETURN_NONE;
	}

	PyObject *set = PySet_New(NULL);

	if(!set)
	{
		Py_RETURN_NONE;
	}

	struct capability *capability = capabilities;

	while(capability < (struct capability *)(capabilities + sizeof(capabilities)))
		{
			if(caps.capabilities & capability->id)
	{
		PyObject *s = PyString_FromString(capability->name);

		if(!s)
			{
				Py_DECREF(set);
				Py_RETURN_NONE;
			}

		PySet_Add(set, s);
	}

			capability++;
		}

	return Py_BuildValue("sssO", caps.driver, caps.card, caps.bus_info, set);
}

static PyObject *Video_device_set_format(Video_device *self, PyObject *args)
{
	int size_x;
	int size_y;
	const char *fmt = NULL;

	if(!PyArg_ParseTuple(args, "ii|s", &size_x, &size_y, &fmt))
		{
			Py_RETURN_NONE;
		}

	struct v4l2_format format;
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = size_x;
	format.fmt.pix.height = size_y;
#ifdef USE_LIBV4L
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if(fmt != NULL && strcmp(fmt, "MJPEG")==0)
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
	if(fmt != NULL && strcmp(fmt, "RGB24")==0)
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
	if(fmt != NULL && strcmp(fmt, "YUV420")==0)
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
#else
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
#endif
	format.fmt.pix.field = V4L2_FIELD_NONE;
	format.fmt.pix.bytesperline = 0;

	if(my_ioctl(self->fd, VIDIOC_S_FMT, &format))
		{
			Py_RETURN_NONE;
		}

	return Py_BuildValue("ii", format.fmt.pix.width, format.fmt.pix.height);
}

static PyObject *Video_device_set_fps(Video_device *self, PyObject *args)
{
	int fps;
	if(!PyArg_ParseTuple(args, "i", &fps))
		{
			Py_RETURN_NONE;
		}
	struct v4l2_streamparm setfps;
	memset(&setfps, 0, sizeof(struct v4l2_streamparm));
	setfps.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	setfps.parm.capture.timeperframe.numerator = 1;
	setfps.parm.capture.timeperframe.denominator = fps;
	if(my_ioctl(self->fd, VIDIOC_S_PARM, &setfps)){
		Py_RETURN_NONE;
	}
	return Py_BuildValue("i",setfps.parm.capture.timeperframe.denominator);
}

static PyObject *Video_device_get_format(Video_device *self)
{

	struct v4l2_format format;
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if(my_ioctl(self->fd, VIDIOC_G_FMT, &format))
	{
		Py_RETURN_NONE;
	}

	PyObject *out = PyTuple_New(3);
	PyTuple_SetItem(out, 0, PyInt_FromLong(format.fmt.pix.width));
	PyTuple_SetItem(out, 1, PyInt_FromLong(format.fmt.pix.height));

	PyObject *pixFormatStr = NULL;
	switch(format.fmt.pix.pixelformat)
	{
	case V4L2_PIX_FMT_MJPEG:
		pixFormatStr = PyString_FromString("MJPEG");
		break;
	case V4L2_PIX_FMT_RGB24:
		pixFormatStr = PyString_FromString("RGB24");
		break;
	case V4L2_PIX_FMT_YUV420:
		pixFormatStr = PyString_FromString("YUV420");
		break;
	case V4L2_PIX_FMT_YUYV:
		pixFormatStr = PyString_FromString("YUYV");
		break;
	default:
		pixFormatStr = PyString_FromString("Unknown");
		break;
	}
	PyTuple_SetItem(out, 2, pixFormatStr);
	return out;

}

static PyObject *Video_device_start(Video_device *self)
{
	ASSERT_OPEN;
	enum v4l2_buf_type type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if(my_ioctl(self->fd, VIDIOC_STREAMON, &type))
		{
			Py_RETURN_NONE;
		}

	Py_RETURN_NONE;
}

static PyObject *Video_device_stop(Video_device *self)
{
	ASSERT_OPEN;
	enum v4l2_buf_type type;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if(my_ioctl(self->fd, VIDIOC_STREAMOFF, &type))
		{
			Py_RETURN_NONE;
		}

	Py_RETURN_NONE;
}

static PyObject *Video_device_create_buffers(Video_device *self, PyObject *args)
{
	int buffer_count;

	if(!PyArg_ParseTuple(args, "I", &buffer_count))
		{
			Py_RETURN_NONE;
		}

	ASSERT_OPEN;

	if(self->buffers)
		{
			PyErr_SetString(PyExc_ValueError, "Buffers are already created");
			Py_RETURN_NONE;
		}

	struct v4l2_requestbuffers reqbuf;
	reqbuf.count = buffer_count;
	reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	if(my_ioctl(self->fd, VIDIOC_REQBUFS, &reqbuf))
		{
			Py_RETURN_NONE;
		}

	if(!reqbuf.count)
		{
			PyErr_SetString(PyExc_IOError, "Not enough buffer memory");
			Py_RETURN_NONE;
		}

	self->buffers = (struct buffer *)malloc(reqbuf.count * sizeof(struct buffer));

	if(!self->buffers)
		{
			PyErr_NoMemory();
			Py_RETURN_NONE;
		}

	unsigned int i;

	for(i = 0; i < reqbuf.count; i++)
		{
			struct v4l2_buffer buffer;
			buffer.index = i;
			buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buffer.memory = V4L2_MEMORY_MMAP;

			if(my_ioctl(self->fd, VIDIOC_QUERYBUF, &buffer))
	{
		Py_RETURN_NONE;
	}

			self->buffers[i].length = buffer.length;
			self->buffers[i].start = v4l2_mmap(NULL, buffer.length,
		PROT_READ | PROT_WRITE, MAP_SHARED, self->fd, buffer.m.offset);

			if(self->buffers[i].start == MAP_FAILED)
	{
		PyErr_SetFromErrno(PyExc_IOError);
		Py_RETURN_NONE;
	}
		}

	self->buffer_count = i;
	Py_RETURN_NONE;
}

static PyObject *Video_device_queue_all_buffers(Video_device *self)
{
	if(!self->buffers)
		{
			ASSERT_OPEN;
			PyErr_SetString(PyExc_ValueError, "Buffers have not been created");
			Py_RETURN_NONE;
		}

	int i;
	int buffer_count = self->buffer_count;

	for(i = 0; i < buffer_count; i++)
		{
			struct v4l2_buffer buffer;
			buffer.index = i;
			buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buffer.memory = V4L2_MEMORY_MMAP;

			if(my_ioctl(self->fd, VIDIOC_QBUF, &buffer))
	{
		Py_RETURN_NONE;
	}
		}

	Py_RETURN_NONE;
}

static PyObject *Video_device_read_internal(Video_device *self, int queue, int return_timestamp)
{
	if(!self->buffers)
		{
			ASSERT_OPEN;
			PyErr_SetString(PyExc_ValueError, "Buffers have not been created");
			Py_RETURN_NONE;
		}

	struct v4l2_buffer buffer;
	buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buffer.memory = V4L2_MEMORY_MMAP;

	if(my_ioctl(self->fd, VIDIOC_DQBUF, &buffer))
		{
			Py_RETURN_NONE;
		}

#ifdef USE_LIBV4L
	PyObject *result = PyString_FromStringAndSize(
			(const char*)self->buffers[buffer.index].start, buffer.bytesused);

	if(!result)
		{
			Py_RETURN_NONE;
		}
#else
	// Convert buffer from YUYV to RGB.
	// For the byte order, see: http://v4l2spec.bytesex.org/spec/r4339.htm
	// For the color conversion, see: http://v4l2spec.bytesex.org/spec/x2123.htm
	int length = buffer.bytesused * 6 / 4;
	PyObject *result = PyString_FromStringAndSize(NULL, length);

	if(!result)
		{
			Py_RETURN_NONE;
		}

	char *rgb = PyString_AS_STRING(result);
	char *rgb_max = rgb + length;
	unsigned char *yuyv = self->buffers[buffer.index].start;

#define CLAMP(c) ((c) <= 0 ? 0 : (c) >= 65025 ? 255 : (c) >> 8)
	while(rgb < rgb_max)
		{
			int u = yuyv[1] - 128;
			int v = yuyv[3] - 128;
			int uv = 100 * u + 208 * v;
			u *= 516;
			v *= 409;

			int y = 298 * (yuyv[0] - 16);
			rgb[0] = CLAMP(y + v);
			rgb[1] = CLAMP(y - uv);
			rgb[2] = CLAMP(y + u);

			y = 298 * (yuyv[2] - 16);
			rgb[3] = CLAMP(y + v);
			rgb[4] = CLAMP(y - uv);
			rgb[5] = CLAMP(y + u);

			rgb += 6;
			yuyv += 4;
		}
#undef CLAMP
#endif

	PyObject *out = result;

	if(return_timestamp)
	{
		out = PyTuple_New(4);
		PyTuple_SetItem(out, 0, result);
		PyTuple_SetItem(out, 1, PyInt_FromLong(buffer.timestamp.tv_sec));
		PyTuple_SetItem(out, 2, PyInt_FromLong(buffer.timestamp.tv_usec));
		PyTuple_SetItem(out, 3, PyInt_FromLong(buffer.sequence));
	}

	if(queue && my_ioctl(self->fd, VIDIOC_QBUF, &buffer))
		{
			Py_RETURN_NONE;
		}

	return out;
}

static PyObject *Video_device_read(Video_device *self, PyObject *args)
{
	int return_timestamp=0;

	if(!PyArg_ParseTuple(args, "|i", &return_timestamp))
		{
			Py_RETURN_NONE;
		}

	return Video_device_read_internal(self, 0, return_timestamp);
}

static PyObject *Video_device_read_and_queue(Video_device *self, PyObject *args)
{
	int return_timestamp=0;

	if(!PyArg_ParseTuple(args, "|i", &return_timestamp))
		{
			Py_RETURN_NONE;
		}

	return Video_device_read_internal(self, 1, return_timestamp);
}

// *********************************************************************

#define HUFFMAN_SEGMENT_LEN 420

const char huffmanSegment[HUFFMAN_SEGMENT_LEN+1] =
	"\xFF\xC4\x01\xA2\x00\x00\x01\x05\x01\x01\x01\x01"
	"\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x01\x02"
	"\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x01\x00\x03"
	"\x01\x01\x01\x01\x01\x01\x01\x01\x01\x00\x00\x00"
	"\x00\x00\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09"
	"\x0A\x0B\x10\x00\x02\x01\x03\x03\x02\x04\x03\x05"
	"\x05\x04\x04\x00\x00\x01\x7D\x01\x02\x03\x00\x04"
	"\x11\x05\x12\x21\x31\x41\x06\x13\x51\x61\x07\x22"
	"\x71\x14\x32\x81\x91\xA1\x08\x23\x42\xB1\xC1\x15"
	"\x52\xD1\xF0\x24\x33\x62\x72\x82\x09\x0A\x16\x17"
	"\x18\x19\x1A\x25\x26\x27\x28\x29\x2A\x34\x35\x36"
	"\x37\x38\x39\x3A\x43\x44\x45\x46\x47\x48\x49\x4A"
	"\x53\x54\x55\x56\x57\x58\x59\x5A\x63\x64\x65\x66"
	"\x67\x68\x69\x6A\x73\x74\x75\x76\x77\x78\x79\x7A"
	"\x83\x84\x85\x86\x87\x88\x89\x8A\x92\x93\x94\x95"
	"\x96\x97\x98\x99\x9A\xA2\xA3\xA4\xA5\xA6\xA7\xA8"
	"\xA9\xAA\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA\xC2"
	"\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xD2\xD3\xD4\xD5"
	"\xD6\xD7\xD8\xD9\xDA\xE1\xE2\xE3\xE4\xE5\xE6\xE7"
	"\xE8\xE9\xEA\xF1\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9"
	"\xFA\x11\x00\x02\x01\x02\x04\x04\x03\x04\x07\x05"
	"\x04\x04\x00\x01\x02\x77\x00\x01\x02\x03\x11\x04"
	"\x05\x21\x31\x06\x12\x41\x51\x07\x61\x71\x13\x22"
	"\x32\x81\x08\x14\x42\x91\xA1\xB1\xC1\x09\x23\x33"
	"\x52\xF0\x15\x62\x72\xD1\x0A\x16\x24\x34\xE1\x25"
	"\xF1\x17\x18\x19\x1A\x26\x27\x28\x29\x2A\x35\x36"
	"\x37\x38\x39\x3A\x43\x44\x45\x46\x47\x48\x49\x4A"
	"\x53\x54\x55\x56\x57\x58\x59\x5A\x63\x64\x65\x66"
	"\x67\x68\x69\x6A\x73\x74\x75\x76\x77\x78\x79\x7A"
	"\x82\x83\x84\x85\x86\x87\x88\x89\x8A\x92\x93\x94"
	"\x95\x96\x97\x98\x99\x9A\xA2\xA3\xA4\xA5\xA6\xA7"
	"\xA8\xA9\xAA\xB2\xB3\xB4\xB5\xB6\xB7\xB8\xB9\xBA"
	"\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xD2\xD3\xD4"
	"\xD5\xD6\xD7\xD8\xD9\xDA\xE2\xE3\xE4\xE5\xE6\xE7"
	"\xE8\xE9\xEA\xF2\xF3\xF4\xF5\xF6\xF7\xF8\xF9\xFA";

int ReadJpegFrame(const unsigned char *data, unsigned offset, const unsigned char **twoBytesOut, unsigned *frameStartPosOut, unsigned *cursorOut)
{
	//Based on http://www.gdcl.co.uk/2013/05/02/Motion-JPEG.html
	//and https://en.wikipedia.org/wiki/JPEG

	*twoBytesOut = NULL;
	*frameStartPosOut = 0;
	*cursorOut = 0;
	unsigned cursor = offset;
	//Check frame start
	unsigned frameStartPos = offset;
	const unsigned char *twoBytes = &data[cursor];

	if (twoBytes[0] != 0xff)
	{
		//print "Error: found header", map(hex,twoBytes),"at position",cursor
		return 0;
	}

	cursor = 2 + cursor;

	//Handle padding
	int paddingByte = (twoBytes[0] == 0xff && twoBytes[1] == 0xff);
	if(paddingByte)
	{
		*twoBytesOut = twoBytes;
		*frameStartPosOut = frameStartPos;
		*cursorOut = cursor;
		return 1;
	}

	//Structure markers with 2 byte length
	int markHeader = (twoBytes[0] == 0xff && twoBytes[1] >= 0xd0 && twoBytes[1] <= 0xd9);
	if (markHeader)
	{
		*twoBytesOut = twoBytes;
		*frameStartPosOut = frameStartPos;
		*cursorOut = cursor;
		return 1;
	}

	//Determine length of compressed (entropy) data
	int compressedDataStart = (twoBytes[0] == 0xff && twoBytes[1] == 0xda);
	if (compressedDataStart)
	{
		unsigned sosLength = ((data[cursor] << 8) + data[cursor+1]);
		cursor += sosLength;

		//Seek through frame
		int run = 1;
		while(run)
		{
			unsigned char byte = data[cursor];
			cursor += 1;
			
			if(byte == 0xff)
			{
				unsigned char byte2 = data[cursor];
				cursor += 1;
				if(byte2 != 0x00)
				{
					if(byte2 >= 0xd0 && byte2 <= 0xd8)
					{
						//Found restart structure
						//print hex(byte), hex(byte2)
					}
					else
					{
						//End of frame
						run = 0;
						cursor -= 2;
					}
				}
				else
				{
					//Add escaped 0xff value in entropy data
				}	
			}
			else
			{
				
			}
		}

		*twoBytesOut = twoBytes;
		*frameStartPosOut = frameStartPos;
		*cursorOut = cursor;
		return 1;
	}

	//More cursor for all other segment types
	unsigned segLength = (data[cursor] << 8) + data[cursor+1];
	cursor += segLength;
	*twoBytesOut = twoBytes;
	*frameStartPosOut = frameStartPos;
	*cursorOut = cursor;
	return 1;
}

static PyObject *InsertHuffmanTable(PyObject *self, PyObject *args)
{
	/* This converts an MJPEG frame into a standard JPEG binary
	MJPEG images omit the huffman table if the standard table
	is used. If it is missing, this function adds the table
	into the file structure. */

	if(PyTuple_Size(args) < 1)
	{
		PyErr_BadArgument();
		PyErr_Format(PyExc_TypeError, "Function requires 1 argument");
 		Py_RETURN_NONE;
	}

	PyObject *inBuffer = PyTuple_GetItem(args, 0);

	if(!PyString_Check(inBuffer))
	{
		PyErr_BadArgument();
		PyErr_Format(PyExc_TypeError, "Argument 1 must be a string.");
		//PyObject* type = PyObject_Type(inBuffer);
		//PyObject_Print(type, stdout, Py_PRINT_RAW);
		//Py_CLEAR(type);
		
 		Py_RETURN_NONE;
	}

	int parsing = 1;
	unsigned frameStartPos = 0;
	int huffFound = 0;
	unsigned char* inBufferPtr = (unsigned char*)PyString_AsString(inBuffer);
	Py_ssize_t inBufferLen = PyString_Size(inBuffer);

	PyObject *outBuffer = PyString_FromString("");

	while(parsing)
	{
		//Check if we should stop
		if (frameStartPos >= inBufferLen)
		{
			parsing = 0;
			continue;
		}

		//Read the next segment
		const unsigned char *twoBytes = NULL;
		unsigned frameEndPos=0;
	
		int ok = ReadJpegFrame(inBufferPtr, frameStartPos, &twoBytes, &frameStartPos, &frameEndPos);

		//if(verbose)
		//	print map(hex, twoBytes), frameStartPos, frameEndPos;

		//Stop if there is a serious error
		if(!ok)
		{
			parsing = 0;
			continue;
		}
	
		//Check if this segment is the compressed data
		if(twoBytes[0] == 0xff && twoBytes[1] == 0xda && !huffFound)
		{
			PyObject *substr = PyString_FromStringAndSize(huffmanSegment, HUFFMAN_SEGMENT_LEN);
			PyString_ConcatAndDel(&outBuffer, substr);
		}

		//Check the type of frame
		if(twoBytes[0] == 0xff && twoBytes[1] == 0xc4)
			huffFound = 1;

		//Write current structure to output
		PyObject *substr = PyString_FromStringAndSize((char *)&inBufferPtr[frameStartPos], frameEndPos - frameStartPos);
		PyString_ConcatAndDel(&outBuffer, substr);

		//Move cursor
		frameStartPos = frameEndPos;
	}

	return outBuffer;
}

// *********************************************************************

class SetFormatParams
{
public:
	std::string fmt;
	int width, height;

	SetFormatParams()
	{
		width = 0;
		height = 0;
	}

	SetFormatParams(const class SetFormatParams &in)
	{
		SetFormatParams::operator=(in);
	}	

	class SetFormatParams &SetFormatParams=(const class SetFormatParams &in)
	{
		width = in.width;
		height = in.height;
		fmt = in.fmt
		return *this;
	}	

};


class Device_manager_Worker_thread_args
{
public:
	Device_manager *self;
	std::string devName;
	int stop;
	int stopped;
	pthread_mutex_t lock;
	std::vector<std::string> openDeviceFlag;
	std::vector<int> startDeviceFlag;
	int stopDeviceFlag;
	int closeDeviceFlag;
	int deviceStarted;
	int fd;
	struct buffer *buffers;
	int buffer_counts;
	std::string pxFmt;

	Device_manager_Worker_thread_args(const char *devNameIn)
	{
		stop = 0;
		stopped = 1;
		deviceStarted = 0;
		this->devName = devNameIn;
		pthread_mutex_init(&lock, NULL);
		buffer_counts = 10;
		buffers = NULL;
		stopDeviceFlag = 0;
		closeDeviceFlag = 0;
	};

	virtual ~Device_manager_Worker_thread_args()
	{
		if(deviceStarted)
		{
			this->StopDeviceInternal();
		}

		if(fd!=-1)
		{
			this->CloseDeviceInternal();
		}

		if(buffers) delete [] buffers;
		this->buffers = NULL;

		pthread_mutex_destroy(&lock);
	};

	void Stop()
	{
		pthread_mutex_lock(&this->lock);
		this->stop = 1;
		pthread_mutex_unlock(&this->lock);
	};

	void WaitForStop()
	{
		while(1)
		{
			pthread_mutex_lock(&this->lock);
			int s = this->stopped;
			pthread_mutex_unlock(&this->lock);

			if(s) return;
			usleep(10000);
		}
	};

	void OpenDevice()
	{
		pthread_mutex_lock(&this->lock);
		this->openDeviceFlag.push_back(this->devName.c_str());
		pthread_mutex_unlock(&this->lock);
	};

	void SetFormat()
	{
		class SetFormatParams params;
		

		pthread_mutex_lock(&this->lock);
		//this->openDeviceFlag.push_back(this->devName.c_str());
		pthread_mutex_unlock(&this->lock);
	}

	void StartDevice(int buffer_count)
	{
		pthread_mutex_lock(&this->lock);
		this->startDeviceFlag.push_back(buffer_count);
		pthread_mutex_unlock(&this->lock);
	};

	void StopDevice()
	{
		pthread_mutex_lock(&this->lock);
		this->stopDeviceFlag = 1;
		pthread_mutex_unlock(&this->lock);
	};

	void CloseDevice()
	{
		pthread_mutex_lock(&this->lock);
		this->closeDeviceFlag = 1;
		pthread_mutex_unlock(&this->lock);
	};

	int ReadFrame()
	{
		if(this->fd<0)
			throw std::runtime_error("File not open");

		if(this->buffers == NULL)
			throw std::runtime_error("Buffers have not been created");

		struct v4l2_buffer buffer;
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;

		if(my_ioctl(this->fd, VIDIOC_DQBUF, &buffer, 10000))
		{
			return 0;
		}

		this->DecodeFrame((const unsigned char*)this->buffers[buffer.index].start, buffer.bytesused);

		//PyObject *out = result;

		if(1)
		{
			/*out = PyTuple_New(4);
			PyTuple_SetItem(out, 0, result);
			PyTuple_SetItem(out, 1, PyInt_FromLong(buffer.timestamp.tv_sec));
			PyTuple_SetItem(out, 2, PyInt_FromLong(buffer.timestamp.tv_usec));
			PyTuple_SetItem(out, 3, PyInt_FromLong(buffer.sequence));*/
		}

		if(my_ioctl(this->fd, VIDIOC_QBUF, &buffer))
		{
			//Py_RETURN_NONE;
		}

		return 1;
	}

	int DecodeFrame(const unsigned char *data, unsigned dataLen)
	{
		printf("rx %d\n", dataLen);
	}

	int OpenDeviceInternal()
	{
		printf("OpenDeviceInternal\n");
		//Open the video device.
		this->fd = v4l2_open(this->devName.c_str(), O_RDWR | O_NONBLOCK);

		if(fd < 0)
		{
			throw std::runtime_error("Error opening device");
		}

		this->deviceStarted = 0;
		printf("Done opening\n");
		return 1;
	}

	int SetFormat(int size_x, int size_y, const char *fmt)
	{
		pthread_mutex_lock(&this->lock);
		struct v4l2_format format;
		format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		format.fmt.pix.width = size_x;
		format.fmt.pix.height = size_y;
	#ifdef USE_LIBV4L
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
		if(fmt != NULL && strcmp(fmt, "MJPEG")==0)
			format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
		if(fmt != NULL && strcmp(fmt, "RGB24")==0)
			format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
		if(fmt != NULL && strcmp(fmt, "YUV420")==0)
			format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	#else
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	#endif
		format.fmt.pix.field = V4L2_FIELD_NONE;
		format.fmt.pix.bytesperline = 0;

		if(my_ioctl(this->fd, VIDIOC_S_FMT, &format))
		{
			pthread_mutex_unlock(&this->lock);
			return 0;
		}

		//Store pixel format for decoding usage later
		this->pxFmt = fmt;
		pthread_mutex_unlock(&this->lock);
		return 1;
	}

	int StartDeviceInternal(int buffer_count = 10)
	{
		printf("StartDeviceInternal\n");
		//Check this device has not already been start
		if(this->fd==-1)
		{
			throw std::runtime_error("Device not open");
		}

		//Set other parameters for capture
		//TODO

		/*
		//Query current pixel format
		self.size_x, self.size_y, self.pixelFmt = self.video.get_format()

		//Set target frames per second
		self.fps = self.video.set_fps(reqFps)
		*/

		// Create a buffer to store image data in. This must be done before
		// calling 'start' if v4l2capture is compiled with libv4l2. Otherwise
		// raises IOError.

		struct v4l2_requestbuffers reqbuf;
		reqbuf.count = buffer_count;
		reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		reqbuf.memory = V4L2_MEMORY_MMAP;

		if(my_ioctl(this->fd, VIDIOC_REQBUFS, &reqbuf))
		{
			throw std::runtime_error("VIDIOC_REQBUFS failed");
		}

		if(!reqbuf.count)
		{
			throw std::runtime_error("Not enough buffer memory");
		}

		this->buffers = new struct buffer [reqbuf.count];

		if(this->buffers == NULL)
		{
			throw std::runtime_error("Failed to allocate buffer memory");
		}

		for(unsigned int i = 0; i < reqbuf.count; i++)
		{
			struct v4l2_buffer buffer;
			buffer.index = i;
			buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buffer.memory = V4L2_MEMORY_MMAP;

			if(my_ioctl(fd, VIDIOC_QUERYBUF, &buffer))
			{
				throw std::runtime_error("VIDIOC_QUERYBUF failed");
			}

			this->buffers[i].length = buffer.length;
			this->buffers[i].start = v4l2_mmap(NULL, buffer.length,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);

			if(this->buffers[i].start == MAP_FAILED)
			{
				throw std::runtime_error("v4l2_mmap failed");
			}
		}

		this->buffer_counts = reqbuf.count;

		// Send the buffer to the device. Some devices require this to be done
		// before calling 'start'.

		for(int i = 0; i < buffer_count; i++)
		{
			struct v4l2_buffer buffer;
			buffer.index = i;
			buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			buffer.memory = V4L2_MEMORY_MMAP;

			if(my_ioctl(fd, VIDIOC_QBUF, &buffer))
			{
				throw std::runtime_error("VIDIOC_QBUF failed");
			}
		}

		// Start the device. This lights the LED if it's a camera that has one.
		enum v4l2_buf_type type;
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if(my_ioctl(fd, VIDIOC_STREAMON, &type))
		{
			throw std::runtime_error("VIDIOC_STREAMON failed");
		}

		this->deviceStarted = 1;
		printf("Started ok\n");
		return 1;
	}

	void StopDeviceInternal()
	{
		printf("StopDeviceInternal\n");
		if(this->fd==-1)
		{
			throw std::runtime_error("Device not started");
		}

		//Signal V4l2 api
		enum v4l2_buf_type type;
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

		if(my_ioctl(this->fd, VIDIOC_STREAMOFF, &type))
		{
			throw std::runtime_error("VIDIOC_STREAMOFF failed");
		}

		this->deviceStarted = 0;
	}

	int CloseDeviceInternal()
	{
		printf("CloseDeviceInternal\n");
		if(this->fd == -1)
		{
			throw std::runtime_error("Device not open");
		}

		if(this->deviceStarted)
			StopDeviceInternal();

		for(int i = 0; i < this->buffer_counts; i++)
		{
			v4l2_munmap(buffers[i].start, buffers[i].length);	
		}
		delete [] this->buffers;
		this->buffers = NULL;

		//Release memory
		v4l2_close(fd);
		fd = -1;
		return 1;
	}

	void Run()
	{
		printf("Thread started: %s\n", this->devName.c_str());
		int running = 1;
		pthread_mutex_lock(&this->lock);
		this->stopped = 0;
		pthread_mutex_unlock(&this->lock);

		try
		{
		while(running)
		{
			//printf("Sleep\n");
			usleep(1000);

			if(deviceStarted) this->ReadFrame();

			pthread_mutex_lock(&this->lock);
			if(this->openDeviceFlag.size() > 0)
			{
				std::string devName = this->openDeviceFlag[this->openDeviceFlag.size()-1];
				this->openDeviceFlag.pop_back();
				this->OpenDeviceInternal();
			}
			pthread_mutex_unlock(&this->lock);

			pthread_mutex_lock(&this->lock);
			if(this->startDeviceFlag.size() > 0 && this->openDeviceFlag.size() == 0)
			{
				int buffer_count = this->startDeviceFlag[this->startDeviceFlag.size()-1];
				this->startDeviceFlag.pop_back();
				this->StartDeviceInternal(buffer_count);
			}
			pthread_mutex_unlock(&this->lock);

			pthread_mutex_lock(&this->lock);
			if(this->stopDeviceFlag 
				&& this->openDeviceFlag.size() == 0 
				&& this->startDeviceFlag.size() == 0)
			{
				this->StopDeviceInternal();
				this->stopDeviceFlag = 0;
			}
			pthread_mutex_unlock(&this->lock);

			pthread_mutex_lock(&this->lock);
			if(this->closeDeviceFlag && !this->stopDeviceFlag 
				&& this->openDeviceFlag.size() == 0 
				&& this->startDeviceFlag.size() == 0)
			{
				this->CloseDeviceInternal();
				this->closeDeviceFlag = 0;
			}
			pthread_mutex_unlock(&this->lock);

			pthread_mutex_lock(&this->lock);
			running = !this->stop;
			pthread_mutex_unlock(&this->lock);
		}
		}
		catch(std::exception &err)
		{
			printf("An exception has occured: %s\n", err.what());
		}

		printf("Thread stopping\n");
		pthread_mutex_lock(&this->lock);
		this->stopped = 1;
		pthread_mutex_unlock(&this->lock);
	};
};

void *Device_manager_Worker_thread(void *arg)
{
	class Device_manager_Worker_thread_args *argobj = (class Device_manager_Worker_thread_args*) arg;
	argobj->Run();

	return NULL;
}

// **********************************************************************

static void Device_manager_dealloc(Device_manager *self)
{
	//Stop high level threads
	for(std::map<std::string, class Device_manager_Worker_thread_args *>::iterator it = self->threadArgStore->begin(); 
		it != self->threadArgStore->end(); it++)
	{
		PyObject *args = PyTuple_New(1);
		PyTuple_SetItem(args, 0, PyString_FromString(it->first.c_str()));
		Device_manager_stop(self, args);
		Py_DECREF(args);
	}

	delete self->threadArgStore;
	self->ob_type->tp_free((PyObject *)self);
}

static int Device_manager_init(Device_manager *self, PyObject *args,
		PyObject *kwargs)
{
	self->threadArgStore = new std::map<std::string, class Device_manager_Worker_thread_args*>;
	return 0;
}

static PyObject *Device_manager_open(Device_manager *self, PyObject *args)
{
	//Process arguments
	const char *devarg = "/dev/video0";
	if(PyTuple_Size(args) >= 1)
	{
		PyObject *pydevarg = PyTuple_GetItem(args, 0);
		devarg = PyString_AsString(pydevarg);
	}

	//Check this device has not already been opened
	std::map<std::string, class Device_manager_Worker_thread_args *>::iterator it = self->threadArgStore->find(devarg);
	if(it!=self->threadArgStore->end())
	{
		PyErr_Format(PyExc_RuntimeError, "Device already opened.");
 		Py_RETURN_NONE;
	}

	pthread_t thread;
	Device_manager_Worker_thread_args *threadArgs = new Device_manager_Worker_thread_args(devarg);
	(*self->threadArgStore)[devarg] = threadArgs;
	threadArgs->self = self;
	pthread_create(&thread, NULL, Device_manager_Worker_thread, threadArgs);

	threadArgs->OpenDevice();

	Py_RETURN_NONE;
}


static PyObject *Device_manager_set_format(Device_manager *self, PyObject *args)
{
	int size_x;
	int size_y;
	const char *fmt = NULL;
	const char *devarg = NULL;

	if(!PyArg_ParseTuple(args, "sii|s", &devarg, &size_x, &size_y, &fmt))
	{
		Py_RETURN_NONE;
	}

	class Device_manager_Worker_thread_args *threadArgs = (*self->threadArgStore)[devarg];
	int ret = threadArgs->SetFormat(size_x, size_y, fmt);

	return Py_BuildValue("i", ret);
}

static PyObject *Device_manager_Start(Device_manager *self, PyObject *args)
{

	//Process arguments
	const char *devarg = "/dev/video0";
	if(PyTuple_Size(args) >= 1)
	{
		PyObject *pydevarg = PyTuple_GetItem(args, 0);
		devarg = PyString_AsString(pydevarg);
	}

	long buffer_count = 10;
	if(PyTuple_Size(args) >= 4)
	{
		PyObject *pybufferarg = PyTuple_GetItem(args, 4);
		buffer_count = PyInt_AsLong(pybufferarg);
	}

	class Device_manager_Worker_thread_args *threadArgs = (*self->threadArgStore)[devarg];
	threadArgs->StartDevice(buffer_count);
	
	Py_RETURN_NONE;
}

static PyObject *Device_manager_stop(Device_manager *self, PyObject *args)
{
	//Process arguments
	const char *devarg = "/dev/video0";
	if(PyTuple_Size(args) >= 1)
	{
		PyObject *pydevarg = PyTuple_GetItem(args, 0);
		devarg = PyString_AsString(pydevarg);
	}

	class Device_manager_Worker_thread_args *threadArgs = (*self->threadArgStore)[devarg];
	threadArgs->StopDevice();

	Py_RETURN_NONE;
}

static PyObject *Device_manager_close(Device_manager *self, PyObject *args)
{
	//Process arguments
	const char *devarg = "/dev/video0";
	if(PyTuple_Size(args) >= 1)
	{
		PyObject *pydevarg = PyTuple_GetItem(args, 0);
		devarg = PyString_AsString(pydevarg);
	}

	class Device_manager_Worker_thread_args *threadArgs = (*self->threadArgStore)[devarg];
	threadArgs->CloseDevice();

	//Stop worker thread
	threadArgs->Stop();

	//Release memeory
	threadArgs->WaitForStop();
	delete threadArgs;
	self->threadArgStore->erase(devarg);

	Py_RETURN_NONE;
}

// *********************************************************************

static PyMethodDef Video_device_methods[] = {
	{"close", (PyCFunction)Video_device_close, METH_NOARGS,
			 "close()\n\n"
			 "Close video device. Subsequent calls to other methods will fail."},
	{"fileno", (PyCFunction)Video_device_fileno, METH_NOARGS,
			 "fileno() -> integer \"file descriptor\".\n\n"
			 "This enables video devices to be passed select.select for waiting "
			 "until a frame is available for reading."},
	{"get_info", (PyCFunction)Video_device_get_info, METH_NOARGS,
			 "get_info() -> driver, card, bus_info, capabilities\n\n"
			 "Returns three strings with information about the video device, and one "
			 "set containing strings identifying the capabilities of the video "
			 "device."},
	{"set_format", (PyCFunction)Video_device_set_format, METH_VARARGS,
			 "set_format(size_x, size_y, pixel_format='RGB24') -> size_x, size_y\n\n"
			 "Request the video device to set image size and format. The device may "
			 "choose another size than requested and will return its choice. The "
			 "pixel format may be either RGB24, YUV420 or MJPEG."},
	{"get_format", (PyCFunction)Video_device_get_format, METH_NOARGS,
			 "get_format() -> size_x, size_y\n\n"},
	{"set_fps", (PyCFunction)Video_device_set_fps, METH_VARARGS,
			 "set_fps(fps) -> fps \n\n"
			 "Request the video device to set frame per seconds.The device may "
			 "choose another frame rate than requested and will return its choice. " },
	{"start", (PyCFunction)Video_device_start, METH_NOARGS,
			 "start()\n\n"
			 "Start video capture."},
	{"stop", (PyCFunction)Video_device_stop, METH_NOARGS,
			 "stop()\n\n"
			 "Stop video capture."},
	{"create_buffers", (PyCFunction)Video_device_create_buffers, METH_VARARGS,
			 "create_buffers(count)\n\n"
			 "Create buffers used for capturing image data. Can only be called once "
			 "for each video device object."},
	{"queue_all_buffers", (PyCFunction)Video_device_queue_all_buffers,
			 METH_NOARGS,
			 "queue_all_buffers()\n\n"
			 "Let the video device fill all buffers created."},
	{"read", (PyCFunction)Video_device_read, METH_VARARGS,
			 "read(get_timestamp) -> string or tuple\n\n"
			 "Reads image data from a buffer that has been filled by the video "
			 "device. The image data is in RGB24, YUV420 or MJPEG format as decided by "
			 "'set_format'. The buffer is removed from the queue. Fails if no buffer "
			 "is filled. Use select.select to check for filled buffers. If "
			 "get_timestamp is true, a tuple is turned containing (sec, microsec, "
			 "sequence number)"},
	{"read_and_queue", (PyCFunction)Video_device_read_and_queue, METH_VARARGS,
			 "read_and_queue(get_timestamp)\n\n"
			 "Same as 'read', but adds the buffer back to the queue so the video "
			 "device can fill it again."},
	{NULL}
};

static PyTypeObject Video_device_type = {
	PyObject_HEAD_INIT(NULL)
			0, "v4l2capture.Video_device", sizeof(Video_device), 0,
			(destructor)Video_device_dealloc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, Py_TPFLAGS_DEFAULT, "Video_device(path)\n\nOpens the video device at "
			"the given path and returns an object that can capture images. The "
			"constructor and all methods except close may raise IOError.", 0, 0, 0,
			0, 0, 0, Video_device_methods, 0, 0, 0, 0, 0, 0, 0,
			(initproc)Video_device_init
};

// *********************************************************************

static PyMethodDef Device_manager_methods[] = {
	{"open", (PyCFunction)Device_manager_open, METH_VARARGS,
			 "open(dev = '\\dev\\video0')\n\n"
			 "Open video capture."},
	{"set_format", (PyCFunction)Device_manager_set_format, METH_VARARGS,
			 "set_format(dev, size_x, size_y, pixel_format='RGB24') -> size_x, size_y\n\n"
			 "Request the video device to set image size and format. The device may "
			 "choose another size than requested and will return its choice. The "
			 "pixel format may be either RGB24, YUV420 or MJPEG."},
	{"start", (PyCFunction)Device_manager_Start, METH_VARARGS,
			 "start(dev = '\\dev\\video0', reqSize=(640, 480), reqFps = 30, fmt = 'MJPEG\', buffer_count = 10)\n\n"
			 "Start video capture."},
	{"stop", (PyCFunction)Device_manager_stop, METH_VARARGS,
			 "stop(dev = '\\dev\\video0')\n\n"
			 "Stop video capture."},
	{"close", (PyCFunction)Device_manager_close, METH_VARARGS,
			 "close(dev = '\\dev\\video0')\n\n"
			 "Close video device. Subsequent calls to other methods will fail."},

	{NULL}
};

static PyTypeObject Device_manager_type = {
	PyObject_HEAD_INIT(NULL)
			0, "v4l2capture.Device_manager", sizeof(Device_manager), 0,
			(destructor)Device_manager_dealloc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, Py_TPFLAGS_DEFAULT, "Device_manager(path)\n\nOpens the video device at "
			"the given path and returns an object that can capture images. The "
			"constructor and all methods except close may raise IOError.", 0, 0, 0,
			0, 0, 0, Device_manager_methods, 0, 0, 0, 0, 0, 0, 0,
			(initproc)Device_manager_init
};

// *********************************************************************

static PyMethodDef module_methods[] = {
	{ "InsertHuffmanTable", (PyCFunction)InsertHuffmanTable, METH_VARARGS, NULL },
	{ NULL, NULL, 0, NULL }
};

PyMODINIT_FUNC initv4l2capture(void)
{
	Video_device_type.tp_new = PyType_GenericNew;
	Device_manager_type.tp_new = PyType_GenericNew;

	if(PyType_Ready(&Video_device_type) < 0)
		{
			return;
		}

	if(PyType_Ready(&Device_manager_type) < 0)
		{
			return;
		}

	PyObject *module = Py_InitModule3("v4l2capture", module_methods,
			"Capture video with video4linux2.");

	if(!module)
		{
			return;
		}

	Py_INCREF(&Video_device_type);
	PyModule_AddObject(module, "Video_device", (PyObject *)&Video_device_type);
	Py_INCREF(&Device_manager_type);
	PyModule_AddObject(module, "Device_manager", (PyObject *)&Device_manager_type);

}
