#pragma once

#include <vector>
#include <d3d11.h>
#include <obs-module.h>
#include <util/windows/ComPtr.hpp>

class DeviceDX11
{
public:
	DeviceDX11() : mDevice(NULL){}
	~DeviceDX11()
	{
		Free();
	}

	void Free()
	{
		mDevice.Clear();
	}

	bool Create(uint32_t adapter, bool onlyWithOutputs = false);
	void EnumerateDevices(bool onlyWithOutputs, std::vector<std::string> *adapters = nullptr);
	ID3D11Device *GetDevice()
	{
		return mDevice;
	}

private:
	ComPtr<ID3D11Device> mDevice;
	std::vector<uint32_t> mAdapters;
};