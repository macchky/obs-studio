#include <CL/cl.hpp>
#include <CL/cl_d3d11.h>
#include <d3d11.h>
#include <util/base.h>

class DeviceOCL
{
public:
	DeviceOCL()
		: mCommandQueue(nullptr)
		, mContext(nullptr)
		, mKernelY(nullptr)
		, mKernelUV(nullptr)
		, mProgram(nullptr)
		, mDeviceID(nullptr)
		, mImageY(nullptr)
		, mImageUV(nullptr)
	{
	}

	~DeviceOCL()
	{
		delete mImageY;
		delete mImageUV;
		delete mKernelY;
		delete mKernelUV;
		delete mProgram;
		delete mCommandQueue;
		delete mDeviceID;
		delete mContext;
	}

	bool Create(ID3D11Device *dx11device = nullptr);
	bool CreateImages(enum video_format format, size_t width, size_t height);
	cl_command_queue GetCommandQueue()
	{
		return mCommandQueue->operator()();
	}

	void WriteImages(size_t w, size_t h, uint8_t *data[], uint32_t linesizes[]);
	int GetPlanes(void **planes)
	{
		planes[0] = mImageY->operator()();
		planes[1] = mImageUV->operator()();
		return 2;
	}

private:
	cl::CommandQueue *mCommandQueue;
	cl::Context      *mContext;
	cl::Kernel       *mKernelY;
	cl::Kernel       *mKernelUV;
	cl::Program      *mProgram;
	cl::Device       *mDeviceID;

	cl::Image2D      *mImageY;
	cl::Image2D      *mImageUV;
	size_t mAlignedW;
	size_t mAlignedH;
	size_t mUVAlignedW;
	size_t mUVAlignedH;

	clCreateFromD3D11Texture2DKHR_fn   clCreateFromD3D11Texture2DKHR;
	clEnqueueAcquireD3D11ObjectsKHR_fn clEnqueueAcquireD3D11ObjectsKHR;
	clEnqueueReleaseD3D11ObjectsKHR_fn clEnqueueReleaseD3D11ObjectsKHR;
};