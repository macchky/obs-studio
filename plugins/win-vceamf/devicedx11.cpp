#include "devicedx11.hpp"
#include <util/platform.h>

// libobs-d3d11 uses IDXGIFactory1. From MSDN:
// Note  Do not mix the use of DXGI 1.0 (IDXGIFactory) and DXGI 1.1 (IDXGIFactory1) in an application.
// Use IDXGIFactory or IDXGIFactory1, but not both in an application.
// https://msdn.microsoft.com/en-us/library/windows/desktop/ff471318%28v=vs.85%29.aspx

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define do_log(level, format, ...) \
	blog(level, "[VCE AMF DX11] " format, ##__VA_ARGS__)

#define SAFERELEASE(x) do{if(x){x->Release(); x = NULL;}}while(0)

static inline uint32_t GetWinVer()
{
	OSVERSIONINFO ovi;
	ovi.dwOSVersionInfoSize = sizeof(ovi);
	GetVersionEx(&ovi);

	return (ovi.dwMajorVersion << 8) | (ovi.dwMinorVersion);
}

static const IID dxgiFactory2 =
{ 0x50c83a1c, 0xe072, 0x4c48, { 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };

void DeviceDX11::EnumerateDevices(bool onlyWithOutputs, std::vector<std::string> *adapters)
{
	HRESULT hr;
	ComPtr<IDXGIFactory1> pFactory;

	mAdapters.clear();

	IID factoryIID = (GetWinVer() >= 0x602) ? dxgiFactory2 :
		__uuidof(IDXGIFactory1);

	hr = CreateDXGIFactory1(factoryIID, (void**)pFactory.Assign());
	if (FAILED(hr))
	{
		do_log(LOG_ERROR, "CreateDXGIFactory failed. Error: %08X", hr);
		return;
	}

	UINT count = 0;
	do_log(LOG_INFO, "List of AMD DX11 devices:");
	while (true)
	{
		ComPtr<IDXGIAdapter1> pAdapter;
		if (pFactory->EnumAdapters1(count, pAdapter.Assign()) == DXGI_ERROR_NOT_FOUND)
		{
			break;
		}

		DXGI_ADAPTER_DESC desc;
		pAdapter->GetDesc(&desc);

		if (desc.VendorId != 0x1002)
		{
			count++;
			continue;
		}

		ComPtr<IDXGIOutput> pOutput;
		if (onlyWithOutputs && pAdapter->EnumOutputs(0, pOutput.Assign()) == DXGI_ERROR_NOT_FOUND)
		{
			count++;
			continue;
		}

		char strDevice[100];
		_snprintf_s(strDevice, sizeof(strDevice), "%X", desc.DeviceId);

		char name[512];
		os_wcs_to_utf8(desc.Description, 0, name, sizeof(name));

		do_log(LOG_INFO, "    %d: Device ID: %s [%s]", mAdapters.size(), strDevice, name);
		if (adapters)
			adapters->push_back(std::string(name));
		mAdapters.push_back(count);
		count++;
	}
}

bool DeviceDX11::Create(uint32_t adapter, bool onlyWithOutputs)
{
	HRESULT hr;
	bool ret = false;

	ComPtr<IDXGIFactory1> pFactory;
	ComPtr<IDXGIAdapter1> pAdapter;
	ComPtr<IDXGIOutput> pOutput;
	ComPtr<ID3D11Device> pD3D11Device;
	ComPtr<ID3D11DeviceContext> pD3D11Context;

	EnumerateDevices(onlyWithOutputs);

	if (mAdapters.size() < adapter)
	{
		do_log(LOG_ERROR, "Adapter index is out of range.");
		return false;
	}
	/*auto id = std::find(mAdapters.begin(), mAdapters.end(), adapter);
	if (id == mAdapters.end())
	{
		do_log(LOG_ERROR, "Invalid adapter index.");
		return false;
	}*/

	adapter = mAdapters[adapter];

	IID factoryIID = (GetWinVer() >= 0x602) ? dxgiFactory2 :
		__uuidof(IDXGIFactory1);

	hr = CreateDXGIFactory1(factoryIID, (void**)pFactory.Assign());
	if (FAILED(hr))
	{
		do_log(LOG_ERROR, "CreateDXGIFactory failed. Error: %08X", hr);
		return false;
	}

	if (pFactory->EnumAdapters1(adapter, pAdapter.Assign()) == DXGI_ERROR_NOT_FOUND)
	{
		do_log(LOG_ERROR, "Adapter %d not found.", adapter);
		goto finish;
	}

	if (SUCCEEDED(pAdapter->EnumOutputs(0, pOutput.Assign())))
	{
		DXGI_OUTPUT_DESC outputDesc;
		pOutput->GetDesc(&outputDesc);
	}
	UINT createDeviceFlags = 0;

	D3D_FEATURE_LEVEL featureLevels[] =
	{
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0
	};
	D3D_FEATURE_LEVEL featureLevel;

	D3D_DRIVER_TYPE eDriverType = pAdapter != NULL ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
	hr = D3D11CreateDevice(pAdapter, eDriverType, NULL, createDeviceFlags, featureLevels, _countof(featureLevels),
		D3D11_SDK_VERSION, &pD3D11Device, &featureLevel, &pD3D11Context);

	if (FAILED(hr))
	{
		do_log(LOG_WARNING, "Failed to create HW DX11.1 device ");
		hr = D3D11CreateDevice(pAdapter, eDriverType, NULL, createDeviceFlags, featureLevels + 1, _countof(featureLevels) - 1,
			D3D11_SDK_VERSION, pD3D11Device.Assign(), &featureLevel, pD3D11Context.Assign());
	}

	if (FAILED(hr))
	{
		do_log(LOG_WARNING, "Failed to created HW DX11 device");
		goto finish;
	}

	mDevice = pD3D11Device.Detach();

	ret = true;

finish:
	return ret;
}