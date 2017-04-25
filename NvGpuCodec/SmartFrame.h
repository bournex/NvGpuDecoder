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
	virtual unsigned char*	NV12()						= 0;	/* get nv12 device data, non-contiguous */
	virtual unsigned int	Width()						= 0;	/* get nv12 width */
	virtual unsigned int	Height()					= 0;	/* get nv12 height */
	virtual unsigned int	Step()						= 0;	/* get nv12 step */
	virtual unsigned int	FrameNo()					= 0;	/* get frame sequence number */

	virtual unsigned int	Tid()						= 0;	/* get which thread this frame belong to */

	virtual ~ISmartFrame() {};
protected:
	friend void intrusive_ptr_add_ref(ISmartFrame * sf) { sf->add_ref(sf); }
	friend void intrusive_ptr_release(ISmartFrame * sf) { sf->release(sf); }

private:
	virtual void add_ref(ISmartFrame * sf) = 0;
	virtual void release(ISmartFrame * sf) = 0;
};

/**
 * Description: boost intrusive smart pointer defined SmartFrame. user should
				always pass 'ISmartFramePtr' object rather than passing 
				'ISmartFramePtr&'£¬'ISmartFramePtr *' or keep the raw pointer 
				returned from 'get' method. the inner reference counter will 
				increase by 1 during object assignment copy construct, and 
				decrease by 1 during object destruct. the NV12, BGRP and 
				SmartFrame object will be destroyed when reference counter 
				decrease to 0. the BGRP buffer in a single batch are continuous.
				these are actually pointers with additional offset to the 
				beginning of a large GPU buffer. and the large buffer won't 
				destroy until all the ISmartFramePtrs linked to it been destroyed.
				acquire the large buffer by calling Base() method.
 */
typedef boost::intrusive_ptr<ISmartFrame> ISmartFramePtr;

/**
 * Description: batch data callback function
 */
typedef void(*FrameBatchRoutine)(ISmartFramePtr *p, unsigned int len, void * invoker);