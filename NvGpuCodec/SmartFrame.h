#pragma once

#include <boost/intrusive_ptr.hpp>

#define PCC_FRAME_MAX_CHAN	4

/**
* Description:	Define the grid program supports image color space.
*/
typedef enum
{
	PCC_CS_GRAY = 0,
	PCC_CS_RGB,
	PCC_CS_BGR,
	PCC_CS_YUV420P,
	PCC_CS_YUV422P,
	PCC_CS_RGBP,
	PCC_CS_BGRP,
} PCC_ColorSpace;

/**
* Description:	Define image type for cloud.
* Members:		width:		Image width in pixels.
*				height:		Image height in pixels.
*				step:		The image step(stride) in bytes of each channel.
*				timeStamp:	Time stamp for marking frame in video.
*				imageData:	Image data pointer of each channel.
*/

struct PCC_Frame
{
	int			   width;
	int			   height;
	int			   step[PCC_FRAME_MAX_CHAN];
	long long	   timeStamp;
	PCC_ColorSpace colorSpace;
	void *		   imageData[PCC_FRAME_MAX_CHAN];
	int			   stepGPU[PCC_FRAME_MAX_CHAN];
	void *		   imageGPU;
};

class ISmartFrame
{
public:
	virtual unsigned char*	Origin()	= 0;	/* get nv12 device data */
	virtual unsigned char*	Thumb()		= 0;	/* get bgrp device data */
	virtual unsigned int	Width()		= 0;	/* get nv12 width */
	virtual unsigned int	Height()	= 0;	/* get nv12 height */
	virtual unsigned int	Step()		= 0;	/* get nv12 step */

	virtual unsigned int	Pid()		= 0;	/* get which thread this frame belong to */
	virtual unsigned int	FrameNo()	= 0;	/* get frame sequence number */

protected:
	friend void intrusive_ptr_add_ref(ISmartFrame * sf) { sf->add_ref(sf); }
	friend void intrusive_ptr_release(ISmartFrame * sf) { sf->release(sf); }

private:
	virtual void add_ref(ISmartFrame * sf) = 0;
	virtual void release(ISmartFrame * sf) = 0;
};

typedef boost::intrusive_ptr<ISmartFrame> ISmartFramePtr;
/**
 * Description: parameter of p require thread safe implementation
 */
typedef void(*FrameBatchRoutine)(ISmartFramePtr *p, unsigned int len, void * invoker);