# NvGpuDecoder

Make sure you have 
- CUDA toolkit 7.5 or later
- boost 1.59 or later  

installed.

CUDA decoder and media source is implemented in NvCodec.h, and sample use case is in NvGpuCodec.cpp

there're 3 classes in NvCodec.h
- NvEncoder
- NvDecoder
- NvMediaSource

Currently NvDecoder and NvMediaSource have implemented. you can rewrite this classes.  
  
FrameBatchPipe is a batch video frame process framework, which is similar to NVidia Deepstream deep learning framework.

sample have tested on Tesla M4/M40/K40/P4 cards