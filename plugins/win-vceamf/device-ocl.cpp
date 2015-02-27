#include <algorithm>
#include "device-ocl.hpp"
#include <util/base.h>
#include <media-io/video-io.h>

#define do_log(level, format, ...) \
	blog(level, "[VCE AMF CL] " format, ##__VA_ARGS__)

#define RETURNIFFAILED(res, msg, ...) \
	do{if(res != CL_SUCCESS) do_log(LOG_ERROR, msg, ##__VA_ARGS__);}while(0)

#define INITPFN(x) \
	x = static_cast<x ## _fn>(clGetExtensionFunctionAddressForPlatform(platformID(), #x));\
	if(!x) { do_log(LOG_ERROR, "Cannot resolve " #x " function"); return false;}

bool DeviceOCL::Create(ID3D11Device *pD3D11Device)
{
	cl_int status = 0;
	cl::Platform platformID;
	std::vector<cl_context_properties> cxtprops;

	std::vector<cl::Platform> platforms;
	cl::Platform::get(&platforms);
	if (platforms.size() == 0)
	{
		do_log(LOG_ERROR, "No platforms found.");
		return false;
	}

	std::string amdplat("Advanced Micro Devices, Inc.");
	bool found = false;
	for (auto platform : platforms)
	{
		std::string info = platform.getInfo<CL_PLATFORM_VENDOR>(&status);
		if (status == CL_SUCCESS)
		{
			if (!info.compare(0, amdplat.size(), amdplat))
			{
				platformID = platform;
				found = true;
				break;
			}
		}
	}

	if (!found)
	{
		do_log(LOG_ERROR, "Failed to find AMD platform.");
		return false;
	}

	std::string exts = platformID.getInfo<CL_PLATFORM_EXTENSIONS>(&status);
	if (!status)
		do_log(LOG_INFO, "CL Platform Extensions: %s", exts.c_str());

	if (pD3D11Device)
	{
		clGetDeviceIDsFromD3D11KHR_fn pClGetDeviceIDsFromD3D11KHR = 
			static_cast<clGetDeviceIDsFromD3D11KHR_fn>
				(clGetExtensionFunctionAddressForPlatform(platformID(),
					"clGetDeviceIDsFromD3D11KHR"));

		if (pClGetDeviceIDsFromD3D11KHR == NULL)
		{
			do_log(LOG_ERROR, "Cannot resolve ClGetDeviceIDsFromD3D11KHR function");
			return false;
		}

		cl_device_id device = 0;
		status = (*pClGetDeviceIDsFromD3D11KHR)(platformID(), CL_D3D11_DEVICE_KHR,
				(void*)pD3D11Device, CL_PREFERRED_DEVICES_FOR_D3D11_KHR,
				1, &device, NULL);
		RETURNIFFAILED(status, "clGetDeviceIDsFromD3D11KHR failed.");

		INITPFN(clCreateFromD3D11Texture2DKHR);
		INITPFN(clEnqueueAcquireD3D11ObjectsKHR);
		INITPFN(clEnqueueReleaseD3D11ObjectsKHR);

		mDeviceID = new cl::Device(device);
		cxtprops.push_back(CL_CONTEXT_D3D11_DEVICE_KHR);
		cxtprops.push_back((cl_context_properties)pD3D11Device);
	}

	if (!mDeviceID)
	{
		std::vector<cl::Device> devices;
		platformID.getDevices(CL_DEVICE_TYPE_GPU, &devices);
		if (devices.size() == 0)
		{
			do_log(LOG_ERROR, "Failed to get device id.");
			return false;
		}
		mDeviceID = new cl::Device(devices[0]);
	}

	cxtprops.push_back(CL_CONTEXT_INTEROP_USER_SYNC);
	cxtprops.push_back(CL_TRUE);

	cxtprops.push_back(CL_CONTEXT_PLATFORM);
	cxtprops.push_back((cl_context_properties)platformID());
	cxtprops.push_back(0);

	mContext = new cl::Context(*mDeviceID, &cxtprops[0], nullptr, nullptr);
	mCommandQueue = new cl::CommandQueue(*mContext, *mDeviceID, 0, &status);
	if (status != CL_SUCCESS)
		do_log(LOG_ERROR, "Failed to create CL command queue.");

	return true;
}

bool DeviceOCL::CreateImages(enum video_format format, size_t width, size_t height)
{
	cl_int status = 0;
	mAlignedW = ((width + (256 - 1)) & ~(256 - 1));
	mAlignedH = (height + 31) & ~31;

	delete mImageY;
	delete mImageUV;

	if (format == VIDEO_FORMAT_NV12)
	{
		mImageY = new cl::Image2D(*mContext, CL_MEM_WRITE_ONLY,
			cl::ImageFormat(CL_R, CL_UNSIGNED_INT8), mAlignedW, mAlignedH, 0,
			nullptr, &status);

		mUVAlignedW = mAlignedW / 2;
		mUVAlignedH = (mAlignedH + 1) / 2;

		mImageUV = new cl::Image2D(*mContext, CL_MEM_WRITE_ONLY,
			cl::ImageFormat(CL_RG, CL_UNSIGNED_INT8), mUVAlignedW, mUVAlignedH, 0,
			nullptr, &status);

		return true;
	}
	return false;
}

void DeviceOCL::WriteImages(size_t w, size_t h, uint8_t *data[], uint32_t linesizes[])
{
	cl_int status = 0;
	cl::size_t<3> origin;
	cl::size_t<3> region;

	if (w > mAlignedW || h > mAlignedH)
	{
		do_log(LOG_ERROR, "Width and/or height out of range.");
		return;
	}

	region[0] = w;
	region[1] = h;
	region[2] = 1;

	status = mCommandQueue->enqueueWriteImage(*mImageY, false,
		origin, region, linesizes[0], 0, data[0], nullptr, nullptr);
	if (status != CL_SUCCESS)
		do_log(LOG_ERROR, "Failed to write to CL image: %d", status);

	region[0] = w / 2;
	region[1] = (h + 1) / 2;
	region[2] = 1;

	status = mCommandQueue->enqueueWriteImage(*mImageUV, false,
		origin, region, linesizes[1], 0, data[1], nullptr, nullptr);
	if (status != CL_SUCCESS)
		do_log(LOG_ERROR, "Failed to write to CL image: %d", status);

	mCommandQueue->finish();
}