/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

// Mostly copy/paste from obs-x264

#include <exception>
#include <algorithm>
#include <queue>
#include <stdio.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>
#include <obs-module.h>
//#include <graphics/graphics-internal.h>
//#include <../libobs-d3d11/d3d11-subsystem.hpp>

#include <components/Component.h>
#include <components/ComponentCaps.h>
#include <components/VideoEncoderVCE.h>
#include <components/VideoEncoderVCECaps.h>
#include <core/Buffer.h>
#include <core/Surface.h>
#include "device-dx11.hpp"
#include "device-ocl.hpp"
#include "conversion.hpp"
#include "VersionHelpers.h" // Local copy

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define STR_FAILED_TO_SET_PROPERTY "Failed to set '%s' property."

#define do_log(level, format, ...) \
	blog(level, "[VCE AMF encoder: '%s'] " format, \
			obs_encoder_get_name(vceamf->encoder), ##__VA_ARGS__)

#define error(format, ...) do_log(LOG_ERROR,   format, ##__VA_ARGS__)
#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define LOGIFFAILED(result, format, ...) \
	do{if(result != AMF_OK) do_log(LOG_WARNING, format, ##__VA_ARGS__);}while(0)
#define RETURNIFFAILED(result, format, ...) \
	do{if(result != AMF_OK) { do_log(LOG_ERROR, format, ##__VA_ARGS__); return false;}}while(0)

/* ------------------------------------------------------------------------- */

#define AMF_PROP_IGNORE_PKT      L"IgnorePacket"

#define S_BITRATE          "bitrate"
#define S_BUF_SIZE         "buffer_size"
#define S_ENGINE           "engine"
#define S_DISCARD_FILLER   "discard_filler"
#define S_BFRAMES          "b_frames"
#define S_PROFILE          "profile"
#define S_PROFILE_LEVEL    "profile_level"
#define S_RCM              "rcm"
#define S_FORCE_HRD        "force_hrd"
#define S_ENABLE_FILLER    "enable_filler"
#define S_QUALITY_PRESET   "quality_preset"
#define S_QP_VALUE         "qp_value"
#define S_QP_CUSTOM        "qp_custom"
#define S_QP_MIN           "qp_min"
#define S_QP_MAX           "qp_max"
#define S_QP_I             "qp_i"
#define S_QP_P             "qp_p"
#define S_QP_B             "qp_b"
#define S_QP_B_DELTA       "qp_b_delta"
#define S_DEVICE_INDEX     "device_index"
#define S_KEYINT_SEC       "keyint_sec"

#define T_BITRATE             obs_module_text("Bitrate")
#define T_BUF_SIZE            obs_module_text("BufferSize")
#define T_ENGINE_TYPE         obs_module_text("Engine")
#define T_ENGINE_HOST         obs_module_text("Engine.Host")
#define T_ENGINE_DX9          obs_module_text("Engine.DX9")
#define T_ENGINE_DX11         obs_module_text("Engine.DX11")
#define T_ENGINE_OCL          obs_module_text("Engine.OCL")
#define T_DISCARD_FILLER      obs_module_text("DiscardFiller")
#define T_BFRAMES             obs_module_text("BFrames")
#define T_PROFILE             obs_module_text("Profile")
#define T_PROFILE_LEVEL       obs_module_text("Profile.Level")
#define T_PROFILE_BASE        obs_module_text("Profile.Base")
#define T_PROFILE_MAIN        obs_module_text("Profile.Main")
#define T_PROFILE_HIGH        obs_module_text("Profile.High")
#define T_RCM                 obs_module_text("RCM")
#define T_FORCE_HRD           obs_module_text("RCM.ForceHRD")
#define T_ENABLE_FILLER       obs_module_text("RCM.Filler")
#define T_RCM_CBR             obs_module_text("RCM.CBR")
#define T_RCM_CQP             obs_module_text("RCM.CQP")
#define T_RCM_PCVBR           obs_module_text("RCM.PCVBR")
#define T_RCM_LCVBR           obs_module_text("RCM.LCVBR")
#define T_QUALITY_PRESET      obs_module_text("QualityPreset")
#define T_QUALITY_SPEED       obs_module_text("QualityPreset.Speed")
#define T_QUALITY_BALANCED    obs_module_text("QualityPreset.Balanced")
#define T_QUALITY_QUALITY     obs_module_text("QualityPreset.Quality")
#define T_QP_VALUE            obs_module_text("RCM.QPValue")
#define T_QP_MIN              obs_module_text("RCM.QPMin")
#define T_QP_MAX              obs_module_text("RCM.QPMax")
#define T_QP_CUSTOM           obs_module_text("RCM.QPCustom")
#define T_QP_I                obs_module_text("RCM.QPI")
#define T_QP_P                obs_module_text("RCM.QPP")
#define T_QP_B                obs_module_text("RCM.QPB")
#define T_QP_B_DELTA          obs_module_text("RCM.QPBDelta")
#define T_DEVICE_INDEX        obs_module_text("DeviceIndex")
#define T_KEYINT_SEC          obs_module_text("KeyIntSeconds")

// Maybe unnecessery
#define MS_TO_100NS      10000

/* ------------------------------------------------------------------------- */
void PrintProps(amf::AMFPropertyStorage *props)
{
	amf_size count = props->GetPropertyCount();
	for (amf_size i = 0; i < count; i++)
	{
		wchar_t wname[1024];
		amf::AMFVariant var;
		char name[1024];
		
		if (AMF_OK != props->GetPropertyAt(i, wname, 1024, &var))
		{
			blog(LOG_INFO, "Failed to get property at %d", i);
			continue;
		}
		os_wcs_to_utf8(wname, 0, name, sizeof(name));

		switch (var.type)
		{
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_EMPTY:
			blog(LOG_INFO, "%s = <empty>", name);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_BOOL:
			blog(LOG_INFO, "%s = <bool>%d", name, var.boolValue);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_INT64:
			blog(LOG_INFO, "%s = %lld", name, var.int64Value);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_DOUBLE:
			blog(LOG_INFO, "%s = %f", name, var.doubleValue);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_STRING:
			blog(LOG_INFO, "%s = <str>%s", name, var.stringValue);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_WSTRING:
			char tmp[1024];
			os_wcs_to_utf8(var.wstringValue, 0, tmp, sizeof(tmp));
			blog(LOG_INFO, "%s = <wstr>%s", name, tmp);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_RECT:
			blog(LOG_INFO, "%s = <rect>%d,%d,%d,%d", name,
				var.rectValue.left, var.rectValue.top, var.rectValue.right, var.rectValue.bottom);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_SIZE:
			blog(LOG_INFO, "%s = <size>%dx%d", name, var.sizeValue.width, var.sizeValue.height);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_RATE:
			blog(LOG_INFO, "%s = <rate>%d/%d", name, var.rateValue.num, var.rateValue.den);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_RATIO:
			blog(LOG_INFO, "%s = <ratio>%d/%d", name, var.ratioValue.num, var.ratioValue.den);
			break;
		case amf::AMF_VARIANT_TYPE::AMF_VARIANT_INTERFACE:
			blog(LOG_INFO, "%s = <interface>", name);
			break;
		default:
			blog(LOG_INFO, "%s = <type %d>", name, var.type);
		}
	}
}

struct nal_t
{
	int i_ref_idc;
	int i_type;

	uint8_t *p_payload;
	int i_payload;
};

enum nal_unit_type_e
{
	NAL_UNKNOWN = 0,
	NAL_SLICE = 1,
	NAL_SLICE_DPA = 2,
	NAL_SLICE_DPB = 3,
	NAL_SLICE_DPC = 4,
	NAL_SLICE_IDR = 5,    /* ref_idc != 0 */
	NAL_SEI = 6,    /* ref_idc == 0 */
	NAL_SPS = 7,
	NAL_PPS = 8,
	NAL_AUD = 9,
	NAL_FILLER = 12,
	/* ref_idc == 0 for 6,9,10,11,12 */
};

enum EngineType
{
	ENGINE_HOST,
	ENGINE_DX9,
	ENGINE_DX11,
	ENGINE_OPENCL,
	ENGINE_OPENGL,
};

class VCEException : public std::exception
{
public:
	VCEException(const char* msg, AMF_RESULT res = AMF_FAIL)
		: std::exception(msg), mResult(res)
	{
	}
	AMF_RESULT result(){ return mResult; }

private:
	AMF_RESULT mResult;
};

struct AMFParams
{
	int width;
	int height;
	int fps_num;
	int fps_den;
	int adapter;

	AMF_VIDEO_ENCODER_PROFILE_ENUM profile;
	AMF_VIDEO_ENCODER_USAGE_ENUM usage;
	amf::AMF_SURFACE_FORMAT surf_fmt;
	video_format video_fmt;

	int profile_level;
	char engine;
	char quality;
	bool discard_filler;

	//TODO for logging
	void *vceamf;
};

typedef struct packetType
{
	DARRAY(uint8_t) packet;
	int64_t pts;
	bool keyframe;
	bool ignore;
} packetType;

class VCEEncoder;
struct win_vceamf {
	obs_encoder_t          *encoder;
	VCEEncoder             *context;
	AMFParams              params;

	//DARRAY(uint8_t)        packet_data;

	uint8_t                *extra_data;
	uint8_t                *sei;

	size_t                 extra_data_size;
	size_t                 sei_size;

	os_performance_token_t *performance_token;

	enum video_colorspace  colorspace;
	enum video_range_type  range;
};

class Observer : public amf::AMFSurfaceObserver
{
public:
	virtual void AMF_STD_CALL OnSurfaceDataRelease(amf::AMFSurface* pSurface)
	{
		UNUSED_PARAMETER(pSurface);
	}

	Observer() {}
	virtual ~Observer() {}

};

class VCEEncoder
{
public:
	VCEEncoder(win_vceamf *vceamf);
	~VCEEncoder();
	bool ApplySettings(AMFParams &params, obs_data_t *settings);
	bool Encode(struct encoder_frame *frame, struct encoder_packet *packet);
	darray GetHeader();

private:
	void ProcessBitstream(amf::AMFBufferPtr buff, packetType &packet);
	bool CreateDX11Texture(ID3D11Texture2D **pTex);
	void Submit();
	void Poll();
	static void *Submit(void *ptr);
	static void *Poller(void *ptr);

	void ClearQueues()
	{
		while (!mPackets.empty())
		{
			packetType p = mPackets.front();
			da_free(p.packet);
			mPackets.pop();
		}

		while (!mSentPackets.empty())
		{
			packetType p = mSentPackets.front();
			da_free(p.packet);
			mSentPackets.pop();
		}
	}

	amf::AMFContextPtr   mContext;
	amf::AMFComponentPtr mEncoder;
	AMFParams            mParams;

	uint32_t mAlignedSurfaceWidth;
	uint32_t mAlignedSurfaceHeight;
	uint32_t mInBuffSize;
	DeviceDX11 mDeviceDX11;
	DeviceOCL  mDeviceOCL;
	ComPtr<ID3D11Texture2D> pTexture;
	uint8_t   *mHostPtr;
	Observer mObserver;

	DARRAY(uint8_t) mHdrPacket;
	std::queue<packetType> mPackets; //TODO memory fragmentation?
	std::queue<packetType> mSentPackets;

	pthread_t       mSubmitThread;
	pthread_t       mPollerThread;
	pthread_mutex_t mPollerMutex;
	os_event_t     *mStopEvent;
	bool            mRequestKey;
	bool            mWin8OrGreater;
};

VCEEncoder::VCEEncoder(win_vceamf *vceamf) //(AMFParams &params)
	: pTexture(nullptr)
	, mRequestKey(true)
	, mHostPtr(nullptr)
{
	AMF_RESULT res;
	mParams = vceamf->params;
	da_init(mHdrPacket);
	mWin8OrGreater = IsWindows8OrGreater();

	//simulate fail
	//throw VCEException("simulate fail");

	if (vceamf->params.engine == ENGINE_DX9 || vceamf->params.engine > ENGINE_OPENCL)
		throw VCEException("Specified engine support is currently unimplemented");

	if (!mDeviceDX11.Create(mParams.adapter, false))
	{
		if (vceamf->params.engine != ENGINE_HOST)
			throw VCEException("DeviceDX11::Create failed");
	}
	else
	{
		// Only Win8+
		if (mWin8OrGreater && !CreateDX11Texture(pTexture.Assign()))
			throw VCEException("CreateDX11Texture failed");
	}

	if (mParams.engine == ENGINE_OPENCL && !mDeviceOCL.Create(mDeviceDX11.GetDevice()))
		throw VCEException("DeviceOCL::Create failed");

	res = AMFCreateContext(&mContext);
	if (res != AMF_OK)
		throw VCEException("AMFCreateContext failed", res);

	//TODO Can has obs' device?
	/*int type = gs_get_device_type();
	if (type != GS_DEVICE_DIRECT3D_11)
		throw VCEException("Device type is not GS_DEVICE_DIRECT3D_11.", res);

	graphics_t *gs = gs_get_context();*/

	// If no AMD D3D11 device present, fallback to whatever AMF uses then
	// and use plain memory buffers.
	if (mDeviceDX11.Valid())
	{
		res = mContext->InitDX11(mDeviceDX11.GetDevice(), amf::AMF_DX11_0);
		if (res != AMF_OK)
			throw VCEException("AMFContext::InitDX11 failed", res);

		if (mParams.engine == ENGINE_OPENCL)
		{
			res = mContext->InitOpenCL(mDeviceOCL.GetCommandQueue());
			if (res != AMF_OK)
				throw VCEException("AMFContext::InitOpenCL failed", res);
		}
	}

	res = AMFCreateComponent(mContext, AMFVideoEncoderVCE_AVC, &mEncoder);
	if(res != AMF_OK)
		throw VCEException("AMFCreateComponent(encoder) failed", res);

	// Set static settings here
	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCONDING);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_USAGE);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE, mParams.profile);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_PROFILE);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_PROFILE_LEVEL, mParams.profile_level);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_PROFILE_LEVEL);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_QUALITY_PRESET, mParams.quality);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_QUALITY_PRESET);

	res = mEncoder->Init(mParams.surf_fmt, mParams.width, mParams.height);
	if(res != AMF_OK)
		throw VCEException("Encoder init failed", res);

	if (pthread_mutex_init(&mPollerMutex, NULL) != 0)
		throw VCEException("Failed to create poller mutex");

	if (os_event_init(&mStopEvent, OS_EVENT_TYPE_MANUAL) != 0)
		throw VCEException("Failed to create stop event");

	//if(pthread_create(&mSubmitThread, NULL, VCEEncoder::Submit, this))
	//	throw VCEException("Failed to start submitter thread.");

	if (pthread_create(&mPollerThread, NULL, VCEEncoder::Poller, this))
		throw VCEException("Failed to start output poller thread");

#pragma region Print few caps etc.
	amf::H264EncoderCapsPtr encCaps;
	if (mEncoder->QueryInterface(amf::AMFH264EncoderCaps::IID(), (void**)&encCaps) == AMF_OK)
	{
		info("Capabilities:");
		char* accelType[] = {
			"NOT_SUPPORTED",
			"HARDWARE",
			"GPU",
			"SOFTWARE"
		};
		info("  Accel type: %s", accelType[(encCaps->GetAccelerationType() + 1) % 4]);
		info("  Max bitrate: %d", encCaps->GetMaxBitrate());
		//info("  Max priority: %d", encCaps->GetMaxSupportedJobPriority());

		dstr str;
		dstr_init(&str);
		for (int i = 0; i < encCaps->GetNumOfSupportedLevels(); i++)
		{
			dstr_catf(&str, "%d ", encCaps->GetLevel(i));
		}
		info("  Levels: %s", str.array);
		dstr_free(&str);

		for (int i = 0; i < encCaps->GetNumOfSupportedProfiles(); i++)
		{
			dstr_catf(&str, "%d ", encCaps->GetProfile(i));
		}
		info("  Profiles: %s", str.array);
		dstr_free(&str);

		amf::AMFIOCapsPtr iocaps;
		encCaps->GetInputCaps(&iocaps);
		info("  Input mem types:");
		for (int i = iocaps->GetNumOfMemoryTypes() - 1; i >= 0; i--)
		{
			bool native;
			amf::AMF_MEMORY_TYPE memType;
			iocaps->GetMemoryTypeAt(i, &memType, &native);
			char str[128];
			os_wcs_to_utf8(amf::AMFGetMemoryTypeName(memType), 0, str, sizeof(str));
			info("    %s, native: %d", str, native);
		}

		amf_int32 imin, imax;
		iocaps->GetWidthRange(&imin, &imax);
		info("  Width min/max: %d/%d", imin, imax);
		iocaps->GetHeightRange(&imin, &imax);
		info("  Height min/max: %d/%d", imin, imax);
	}

#pragma endregion
}

VCEEncoder::~VCEEncoder()
{
	void *thread_ret;
	os_event_signal(mStopEvent);
	pthread_join(mPollerThread, &thread_ret);

	pTexture.Clear();
	da_free(mHdrPacket);
	ClearQueues();
	delete[] mHostPtr;

	os_event_destroy(mStopEvent);
	pthread_mutex_destroy(&mPollerMutex);
}

bool VCEEncoder::ApplySettings(AMFParams &params, obs_data_t *settings)
{
	AMF_RESULT res = AMF_OK;
	struct win_vceamf *vceamf = (struct win_vceamf *)mParams.vceamf;

	if (mParams.engine != params.engine)
	{
		error("Cannot change engine type of an already initialized encoder context.");
		return false;
	}

	if (mParams.surf_fmt != params.surf_fmt)
	{
		//TODO Right?
		error("Cannot change surface format of an already initialized encoder context.");
		return false;
	}

	if (mParams.engine == ENGINE_OPENCL &&
		!mDeviceOCL.CreateImages(params.video_fmt, params.width, params.height))
	{
		error("Failed to create CL images of type %d, %dx%d.",
				params.video_fmt, params.width, params.height);
		return false;
	}

	int keyint = (int)obs_data_get_int(settings, S_KEYINT_SEC);
	if (keyint == 0)
		keyint = 2;

	double idr = (double(params.fps_num) / double(params.fps_den)) * keyint;
	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, idr);
	RETURNIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_IDR_PERIOD);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
			obs_data_get_int(settings, S_RCM));
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_ENFORCE_HRD,
			obs_data_get_bool(settings, S_FORCE_HRD));
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_ENFORCE_HRD);

	mEncoder->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE,
			obs_data_get_bool(settings, S_ENABLE_FILLER));
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE);

	int bitrate = (int)obs_data_get_int(settings, S_BITRATE);
	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bitrate * 1000);
	RETURNIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_TARGET_BITRATE);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bitrate * 1000);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_PEAK_BITRATE);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
			(int)obs_data_get_int(settings, S_BUF_SIZE) * 1000);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE,
			AMFConstructRate(params.fps_num, params.fps_den));
	RETURNIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_FRAMERATE);

	int qp = (int)obs_data_get_int(settings, S_QP_VALUE);
	int qpI, qpP, qpB, qpBDelta = 4;

	if (obs_data_get_bool(settings, S_QP_CUSTOM))
	{
		qpI = (int)obs_data_get_int(settings, S_QP_I);
		qpP = (int)obs_data_get_int(settings, S_QP_P);
		qpB = (int)obs_data_get_int(settings, S_QP_B);
		qpBDelta = (int)obs_data_get_int(settings, S_QP_B_DELTA);
	}
	else
	{
		qpI = qpP = qpB = qp;
	}

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_QP_I, qpI);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_QP_I);
	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_QP_P, qpP);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_QP_P);
	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_QP_B, qpB);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_QP_B);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_DELTA_QP, qpBDelta);
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_B_PIC_DELTA_QP);

	//res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_REF_B_PIC_DELTA_QP, qpBDelta);
	//LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_REF_B_PIC_DELTA_QP);

	res = mEncoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN,
			obs_data_get_int(settings, S_BFRAMES));
	LOGIFFAILED(res, STR_FAILED_TO_SET_PROPERTY, AMF_VIDEO_ENCODER_B_PIC_PATTERN);

	PrintProps(mEncoder);
	mParams = params;
	return true;
}

darray VCEEncoder::GetHeader()
{
	//XXX Nasty hack. The whole thing. If only mEncoder->GetProperty(L"ExtraData") worked.
	if (!mHdrPacket.num)
	{
		AMF_RESULT res;
		amf::AMFSurfacePtr pSurf;
		amf::AMFDataPtr data;

		if (mParams.video_fmt != VIDEO_FORMAT_NV12)
		{
			blog(LOG_ERROR, "Unimplemented surface format.");
			return mHdrPacket.da;
		}

		size_t y_size = mParams.width * mParams.height;
		delete[] mHostPtr;
		mHostPtr = new uint8_t[y_size * 3 / 2];

		// Optionally un-green the buffer.
		//if (mParams.video_fmt == VIDEO_FORMAT_NV12)
		{
			memset(mHostPtr, 0, y_size);
			memset(mHostPtr + y_size, 128, y_size / 2);
		}

		res = mContext->CreateSurfaceFromHostNative(mParams.surf_fmt,
			mParams.width, mParams.height, mParams.width, mParams.height,
			mHostPtr, &pSurf, nullptr);
		if (res != AMF_OK)
			return mHdrPacket.da;

		pSurf->SetPts(0);
		pSurf->SetProperty(AMF_PROP_IGNORE_PKT, true);
		res = pSurf->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
				AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);

		amf_int64 hdrSpacing = 0;
		mEncoder->GetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, &hdrSpacing);
		mEncoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, 1);
		res = mEncoder->SubmitInput(pSurf);

		while (!mHdrPacket.num)
		{
			os_sleep_ms(5);
			pthread_mutex_lock(&mPollerMutex);
			res = mEncoder->SubmitInput(pSurf);
			pthread_mutex_unlock(&mPollerMutex);
			if (res != AMF_INPUT_FULL && res != AMF_OK)
				break;
		}

		mEncoder->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, hdrSpacing);

		// EOFs despite ReInit
		/*while (mEncoder->Drain() == AMF_INPUT_FULL)
		{
			mEncoder->QueryOutput(&data);
			os_sleep_ms(1);
		}*/

		// Synchronize, let poller copy all header data
		/*pthread_mutex_lock(&mPollerMutex);
		ClearQueues();
		do
		{
			res = mEncoder->QueryOutput(&data);
		} while (res == AMF_OK);
		pthread_mutex_unlock(&mPollerMutex);*/

		res = mEncoder->ReInit(mParams.width, mParams.height);
		if (res != AMF_OK)
			blog(LOG_ERROR, "Failed to reinit the encoder.");
	}
	return mHdrPacket.da;
}

bool VCEEncoder::Encode(struct encoder_frame *frame, struct encoder_packet *packet)
{
	AMF_RESULT res = AMF_OK;
	amf::AMFSurfacePtr pSurf;

	if (mParams.engine == ENGINE_DX11 && mWin8OrGreater)
	{
		if (!pTexture)
		{
			blog(LOG_ERROR, "D3D11 texture is null.");
			return false;
		}

		ID3D11DeviceContext *d3dcontext = nullptr;
		mDeviceDX11.GetDevice()->GetImmediateContext(&d3dcontext);
		if (!d3dcontext)
		{
			blog(LOG_ERROR, "Failed to get immediate D3D11 context.");
			return false;
		}

		D3D11_MAPPED_SUBRESOURCE map;
		HRESULT hr = d3dcontext->Map(pTexture, 0, D3D11_MAP::D3D11_MAP_WRITE_DISCARD, 0, &map);
		if (hr == E_OUTOFMEMORY)
		{
			blog(LOG_ERROR, "Failed to map D3D11 texture: Out of memory.");
			return false;
		}

		if (FAILED(hr))
		{
			blog(LOG_ERROR, "Failed to map D3D11 texture.");
			return false;
		}

		CopyNV12(frame, (uint8_t *)map.pData, mParams.width, mParams.height,
			map.RowPitch, mParams.height);

		d3dcontext->Unmap(pTexture, 0);
		d3dcontext->Release();

		res = mContext->CreateSurfaceFromDX11Native(pTexture, &pSurf, &mObserver);
		if (res != AMF_OK)
		{
			blog(LOG_ERROR, "Failed to create surface from D3D11 texture.");
			return false;
		}
	}
	else if (mParams.engine == ENGINE_OPENCL)
	{
		mDeviceOCL.WriteImages(mParams.width, mParams.height,
				frame->data, frame->linesize);

		void *planes[2];
		mDeviceOCL.GetPlanes(planes);
		res = mContext->CreateSurfaceFromOpenCLNative(mParams.surf_fmt,
			mParams.width, mParams.height, planes, &pSurf, &mObserver);
		if (res != AMF_OK)
		{
			blog(LOG_ERROR, "Failed to create surface from OpenCl images.");
			return false;
		}
	}
	else
	{
		res = mContext->AllocSurface(amf::AMF_MEMORY_HOST, mParams.surf_fmt,
			mParams.width, mParams.height, &pSurf);
		if (res != AMF_OK)
		{
			blog(LOG_ERROR, "Failed to create surface from host buffer.");
			return false;
		}
		amf::AMFPlanePtr plane = pSurf->GetPlaneAt(0);
		CopyNV12(frame, (uint8_t*)plane->GetNative(), mParams.width,
			mParams.height, plane->GetHPitch(), plane->GetVPitch());
	}

	pSurf->SetPts(frame->pts * MS_TO_100NS);
	//pSurf->SetProperty(AMF_PROP_IGNORE_PKT, false);
	if (mRequestKey)
	{
		pSurf->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
			AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
		mRequestKey = false;
	}

	res = mEncoder->SubmitInput(pSurf);

	while (res == AMF_INPUT_FULL)
	{
		os_sleep_ms(1);
		res = mEncoder->SubmitInput(pSurf);
	}

	pthread_mutex_lock(&mPollerMutex);
	while (!mSentPackets.empty())
	{
		packetType pt = mSentPackets.front();
		mSentPackets.pop();
		da_free(pt.packet);
	}

	while (!mPackets.empty())
	{
		packetType pt = mPackets.front();
		mPackets.pop();
		//XXX Nasty hack. Remnants from header parsing
		if (pt.ignore)
		{
			da_free(pt.packet);
			continue;
		}
		mSentPackets.push(pt);

		packet->data = pt.packet.array;
		packet->size = pt.packet.num;
		packet->type = OBS_ENCODER_VIDEO;
		packet->pts = pt.pts;
		packet->dts = pt.pts;
		packet->keyframe = pt.keyframe;
		pthread_mutex_unlock(&mPollerMutex);
		return true;
	}
	pthread_mutex_unlock(&mPollerMutex);

	return false;
}

void *VCEEncoder::Submit(void *ptr)
{
	os_set_thread_name("win-vceamf: submitter thread");
	VCEEncoder *encoder = static_cast<VCEEncoder *>(ptr);
	encoder->Submit();
	return NULL;
}

void *VCEEncoder::Poller(void *ptr)
{
	os_set_thread_name("win-vceamf: output poller thread");
	VCEEncoder *encoder = static_cast<VCEEncoder *>(ptr);
	encoder->Poll();
	return NULL;
}

void VCEEncoder::Submit()
{
	//TODO maybe add separate surface submitter
}

void VCEEncoder::Poll()
{
	AMF_RESULT res = AMF_REPEAT;
	amf::AMFDataPtr data;

	while (os_event_try(mStopEvent) == EAGAIN) {
		res = mEncoder->QueryOutput(&data);

		if (res == AMF_OK)
		{
			pthread_mutex_lock(&mPollerMutex);
			amf::AMFBufferPtr buffer(data);
			packetType pt;
			ProcessBitstream(buffer, pt);
			mPackets.push(pt);
			pthread_mutex_unlock(&mPollerMutex);
		}
		else
		{
			os_sleep_ms(1);
		}
	}
}

void VCEEncoder::ProcessBitstream(amf::AMFBufferPtr buff, packetType &pt)
{
	uint8_t *start = (uint8_t *)buff->GetNative();
	uint8_t *end = start + buff->GetSize();
	const static uint8_t start_seq[] = { 0, 0, 1 };
	start = std::search(start, end, start_seq, start_seq + 3);

	int frameType = -1;
	buff->GetProperty(L"OutputDataType", &frameType);
	pt.keyframe = (frameType == 0);
	pt.pts = buff->GetPts() / MS_TO_100NS;

	pt.ignore = false;
	buff->GetProperty(AMF_PROP_IGNORE_PKT, &pt.ignore);

	da_init(pt.packet);
	bool hasIDR = false;
	bool parseHdr = !mHdrPacket.num;

	while (start != end)
	{
		decltype(start) next = std::search(start + 1, end, start_seq, start_seq + 3);

		nal_t nal;
		nal.i_ref_idc = (start[3] >> 5) & 3;
		nal.i_type = start[3] & 0x1f;
		nal.p_payload = start;
		nal.i_payload = int(next - start);

		//blog(LOG_INFO, "nal type: %d ref_idc: %d", nal.i_type, nal.i_ref_idc);

		//TODO Any difference between SPS/PPS with IDR or with B slice?
		if (parseHdr && (nal.i_type == NAL_SPS || nal.i_type == NAL_PPS))
		{
			da_push_back_array(mHdrPacket, nal.p_payload, nal.i_payload);
		}

		if (nal.i_type == NAL_SLICE_IDR)
			hasIDR = true;
		else if (mParams.discard_filler && nal.i_type == NAL_FILLER)
		{
			start = next;
			continue;
		}

		da_push_back_array(pt.packet, nal.p_payload, nal.i_payload);
		start = next;
	}

	if (!hasIDR)
		da_free(mHdrPacket);
}

bool VCEEncoder::CreateDX11Texture(ID3D11Texture2D **pTex)
{
	HRESULT hres = S_OK;
	D3D11_TEXTURE2D_DESC desc;
	memset(&desc, 0, sizeof(D3D11_TEXTURE2D_DESC));

	desc.Format = DXGI_FORMAT_NV12;
	desc.Width = mParams.width;
	desc.Height = mParams.height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.SampleDesc.Count = 1;
	//desc.Usage = D3D11_USAGE_STAGING;
	// Dynamic for D3D11_MAP_WRITE_DISCARD, faster?
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.Usage = D3D11_USAGE_DYNAMIC;

	ID3D11DeviceContext *d3dcontext = nullptr;

	mDeviceDX11.GetDevice()->GetImmediateContext(&d3dcontext);
	if (!d3dcontext)
	{
		blog(LOG_ERROR, "Failed to get D3D11 immediate context.");
		return false;
	}

	hres = mDeviceDX11.GetDevice()->CreateTexture2D(&desc, 0, pTex);
	if(FAILED(hres))
		blog(LOG_ERROR, "Failed to create D3D11 texture.");

	hres = d3dcontext->Release();
	return true;
}

/* ------------------------------------------------------------------------- */

static const char *win_vceamf_getname(void)
{
	return "VCE AMF";
}

static void clear_data(struct win_vceamf *vceamf)
{
	bfree(vceamf->extra_data);
	bfree(vceamf->sei);

	vceamf->extra_data = NULL;
	vceamf->sei = NULL;
}

static void win_vceamf_destroy(void *data)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);

	if (vceamf) {
		delete vceamf->context;
		os_end_high_performance(vceamf->performance_token);
		clear_data(vceamf);
		bfree(vceamf);
	}
}

static void win_vceamf_defaults(obs_data_t *settings)
{
	obs_data_set_default_int   (settings, S_BITRATE, 5000);
	obs_data_set_default_int   (settings, S_BUF_SIZE, 5000);
	obs_data_set_default_int   (settings, S_QP_VALUE, 25);
	obs_data_set_default_int   (settings, S_QP_MIN, 18);
	obs_data_set_default_int   (settings, S_QP_MAX, 51);
	obs_data_set_default_int   (settings, S_QP_I, 25);
	obs_data_set_default_int   (settings, S_QP_P, 25);
	obs_data_set_default_int   (settings, S_QP_B, 25);
	obs_data_set_default_int   (settings, S_QP_B_DELTA, 4);
	obs_data_set_default_int   (settings, S_RCM,
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
	obs_data_set_default_int   (settings, S_KEYINT_SEC, 2);
	obs_data_set_default_int   (settings, S_BFRAMES, 0);
	obs_data_set_default_int   (settings, S_ENGINE, ENGINE_OPENCL);
	obs_data_set_default_int   (settings, S_PROFILE,
			AMF_VIDEO_ENCODER_PROFILE_MAIN);
	obs_data_set_default_int   (settings, S_PROFILE_LEVEL, 41);
	obs_data_set_default_int   (settings, S_QUALITY_PRESET,
			AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
	obs_data_set_default_bool  (settings, S_ENABLE_FILLER, true);
	obs_data_set_default_bool  (settings, S_FORCE_HRD, true);
	obs_data_set_default_bool  (settings, S_DISCARD_FILLER, false);
}

static inline void add_strings(obs_property_t *list, const char *const *strings)
{
	while (*strings) {
		obs_property_list_add_string(list, *strings, *strings);
		strings++;
	}
}

static bool rcm_modified(obs_properties_t *ppts, obs_property_t *p,
	obs_data_t *settings)
{
	int rcm = (int)obs_data_get_int(settings, S_RCM);
	bool qp_custom = obs_data_get_bool(settings, S_QP_CUSTOM);
	bool showQPs = (rcm == AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTRAINED_QP);
	p = obs_properties_get(ppts, S_BITRATE);
	obs_property_set_visible(p, !showQPs);
	p = obs_properties_get(ppts, S_QP_VALUE);
	obs_property_set_visible(p, showQPs && !qp_custom);
	p = obs_properties_get(ppts, S_QP_I);
	obs_property_set_visible(p, showQPs && qp_custom);
	p = obs_properties_get(ppts, S_QP_P);
	obs_property_set_visible(p, showQPs && qp_custom);
	p = obs_properties_get(ppts, S_QP_B);
	obs_property_set_visible(p, showQPs && qp_custom);
	//p = obs_properties_get(ppts, S_QP_B_DELTA);
	//obs_property_set_visible(p, showQPs && qp_custom);
	return true;
}

static bool engine_type_modified(obs_properties_t *ppts, obs_property_t *p,
	obs_data_t *settings)
{
	int engine = (int)obs_data_get_int(settings, S_ENGINE);
	int oldindex = (int)obs_data_get_int(settings, S_DEVICE_INDEX);
	p = obs_properties_get(ppts, S_DEVICE_INDEX);
	obs_property_list_clear(p);

	if (!(engine == ENGINE_DX9 || engine > ENGINE_OPENCL))
	{
		DeviceDX11 device;
		std::vector<std::string> adapters;
		device.EnumerateDevices(false, &adapters);
		int index = 0;
		for (auto adapter : adapters)
		{
			obs_property_list_add_int(p, adapter.c_str(), index);
			index++;
		}

		if (oldindex >= index)
			oldindex = 0;
		obs_data_set_int(settings, S_DEVICE_INDEX, oldindex);
	}

	return true;
}

static obs_properties_t *win_vceamf_props(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
	obs_property_t *list, *p;

	obs_properties_add_int(props, S_BITRATE, T_BITRATE, 50, 100000, 1);
	obs_properties_add_int(props, S_BUF_SIZE, T_BUF_SIZE, 50, 100000, 1);

	p = obs_properties_add_int(props, S_QP_VALUE, T_QP_VALUE, 0, 51, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, S_QP_I, T_QP_I, 0, 51, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, S_QP_P, T_QP_P, 0, 51, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, S_QP_B, T_QP_B, 0, 51, 1);
	obs_property_set_visible(p, false);
	p = obs_properties_add_int(props, S_QP_B_DELTA, T_QP_B_DELTA, 0, 51, 1);
	//obs_property_set_visible(p, false);

	obs_properties_add_int(props, S_QP_MIN, T_QP_MIN, 0, 51, 1);
	obs_properties_add_int(props, S_QP_MAX, T_QP_MAX, 0, 51, 1);
	obs_properties_add_int(props, S_KEYINT_SEC, T_KEYINT_SEC, 0, 20, 1);

	list = obs_properties_add_list(props, S_RCM, T_RCM,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, T_RCM_CQP,
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTRAINED_QP);
	obs_property_list_add_int(list, T_RCM_CBR,
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
	obs_property_list_add_int(list, T_RCM_PCVBR,
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_PEAK_CONSTRAINED_VBR);
	obs_property_list_add_int(list, T_RCM_LCVBR,
			AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_LATENCY_CONSTRAINED_VBR);
	obs_property_set_modified_callback(list, rcm_modified);

	list = obs_properties_add_list(props, S_ENGINE, T_ENGINE_TYPE,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, T_ENGINE_OCL, ENGINE_OPENCL);
	obs_property_list_add_int(list, T_ENGINE_DX11, ENGINE_DX11);
	obs_property_list_add_int(list, T_ENGINE_DX9, ENGINE_DX9);
	obs_property_list_add_int(list, T_ENGINE_HOST, ENGINE_HOST);
	obs_property_set_modified_callback(list, engine_type_modified);

	list = obs_properties_add_list(props, S_DEVICE_INDEX, T_DEVICE_INDEX,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	list = obs_properties_add_list(props, S_PROFILE, T_PROFILE,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, T_PROFILE_BASE,
			AMF_VIDEO_ENCODER_PROFILE_BASELINE);
	obs_property_list_add_int(list, T_PROFILE_MAIN,
			AMF_VIDEO_ENCODER_PROFILE_MAIN);
	obs_property_list_add_int(list, T_PROFILE_HIGH,
			AMF_VIDEO_ENCODER_PROFILE_HIGH);

	list = obs_properties_add_list(props, S_PROFILE_LEVEL, T_PROFILE_LEVEL,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "3.0", 30);
	obs_property_list_add_int(list, "3.1", 31);
	obs_property_list_add_int(list, "3.2", 32);
	obs_property_list_add_int(list, "4.1", 41);
	obs_property_list_add_int(list, "4.2", 42);
	obs_property_list_add_int(list, "5.0", 50);
	obs_property_list_add_int(list, "5.1", 51);
	//obs_property_list_add_int(list, "5.2", 52);

	list = obs_properties_add_list(props, S_QUALITY_PRESET, T_QUALITY_PRESET,
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, T_QUALITY_BALANCED,
			AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED);
	obs_property_list_add_int(list, T_QUALITY_SPEED,
			AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED);
	obs_property_list_add_int(list, T_QUALITY_QUALITY,
			AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY);

	obs_properties_add_int(props, S_BFRAMES, T_BFRAMES, 0, 16, 1);
	p = obs_properties_add_bool(props, S_QP_CUSTOM, T_QP_CUSTOM);
	obs_property_set_modified_callback(p, rcm_modified);
	obs_properties_add_bool(props, S_FORCE_HRD, T_FORCE_HRD);
	obs_properties_add_bool(props, S_ENABLE_FILLER, T_ENABLE_FILLER);
	obs_properties_add_bool(props, S_DISCARD_FILLER, T_DISCARD_FILLER);

	return props;
}

static inline void set_param(struct win_vceamf *vceamf, const char *param)
{
	UNUSED_PARAMETER(vceamf);
	UNUSED_PARAMETER(param);
}

static void log_vce(void *param, int level, const char *format, va_list args)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(param);
	char str[1024];

	vsnprintf(str, 1024, format, args);
	info("%s", str);

	UNUSED_PARAMETER(level);
}

static void update_params(struct win_vceamf *vceamf, obs_data_t *settings,
		char **params)
{
	video_t *video = obs_encoder_video(vceamf->encoder);
	const struct video_output_info *voi = video_output_get_info(video);

	vceamf->params.engine = (char)obs_data_get_int(settings, S_ENGINE);
	vceamf->params.profile = (AMF_VIDEO_ENCODER_PROFILE_ENUM)
			obs_data_get_int(settings, S_PROFILE);
	vceamf->params.profile_level = (int)obs_data_get_int(settings, S_PROFILE_LEVEL);
	vceamf->params.adapter = (int)obs_data_get_int(settings, S_DEVICE_INDEX);

	vceamf->params.fps_num = voi->fps_num;
	vceamf->params.fps_den = voi->fps_den;
	vceamf->params.width = (int)obs_encoder_get_width(vceamf->encoder);
	vceamf->params.height = (int)obs_encoder_get_height(vceamf->encoder);

	if (voi->format == VIDEO_FORMAT_NV12)
		vceamf->params.surf_fmt = amf::AMF_SURFACE_NV12;
	//else if (voi->format == VIDEO_FORMAT_I420)
	//	vceamf->params.surf_fmt = amf::AMF_SURFACE_YUV420P;
	else
		vceamf->params.surf_fmt = amf::AMF_SURFACE_NV12;
	vceamf->params.video_fmt = voi->format;
	UNUSED_PARAMETER(params);
}

static bool update_settings(struct win_vceamf *vceamf, obs_data_t *settings)
{
	update_params(vceamf, settings, NULL);
	bool success = true;
	if (vceamf->context)
		success = vceamf->context->ApplySettings(vceamf->params, settings);
	return success;
}

static bool win_vceamf_update(void *data, obs_data_t *settings)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);
	return update_settings(vceamf, settings);
}

static void load_headers(struct win_vceamf *vceamf)
{
	DARRAY(uint8_t) header;
	DARRAY(uint8_t) sei;

	da_init(header);
	da_init(sei);

	if (!vceamf->context)
		return;

	darray hdr = vceamf->context->GetHeader();
	if (!hdr.num)
	{
		error("Failed to get headers.");
		return;
	}

	da_push_back_array(header, hdr.array, hdr.num);
	vceamf->extra_data      = header.array;
	vceamf->extra_data_size = header.num;
	vceamf->sei             = sei.array;
	vceamf->sei_size        = sei.num;
}

static void *win_vceamf_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	struct win_vceamf *vceamf = (struct win_vceamf *)bzalloc(sizeof(struct win_vceamf));
	//TODO for logging macros within VCEEncoder. Maybe unclassify the whole thing.
	vceamf->params.vceamf = vceamf;
	vceamf->encoder = encoder;

	if (update_settings(vceamf, settings)) {
		try
		{
			vceamf->context = new VCEEncoder(vceamf);
			if (!update_settings(vceamf, settings))
			{
				delete vceamf->context;
				vceamf->context = nullptr;
				warn("bad settings specified");
			}
		}
		catch (VCEException &ex)
		{
			char res[128];
			os_wcs_to_utf8(amf::AMFGetResultText(ex.result()), 0, res, sizeof(res));
			warn("%s : %d", ex.what(), res);
		}

		if (vceamf->context == NULL)
			warn("vce amf failed to load");
		else
			load_headers(vceamf);
	} else {
		warn("bad settings specified");
	}

	if (!vceamf->context) {
		bfree(vceamf);
		return NULL;
	}

	vceamf->performance_token =
		os_request_high_performance("vce amf encoding");

	return vceamf;
}

static bool win_vceamf_encode(void *data, struct encoder_frame *frame,
		struct encoder_packet *packet, bool *received_packet)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);
	VCEEncoder *encoder = vceamf->context;

	if (!frame || !packet || !received_packet || !encoder)
		return false;

	//TODO Encode
	if (frame)
	{
		*received_packet = encoder->Encode(frame, packet);
		info("received_packet: %d\n", *received_packet);
	}

	return true;
}

static bool win_vceamf_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);

	if (!vceamf->context)
		return false;

	*extra_data = vceamf->extra_data;
	*size       = vceamf->extra_data_size;
	return true;
}

static bool win_vceamf_sei(void *data, uint8_t **sei, size_t *size)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);
	UNUSED_PARAMETER(vceamf);
	UNUSED_PARAMETER(sei);
	UNUSED_PARAMETER(size);
	return false;
	/*if (!vceamf->context)
		return false;

	*sei  = vceamf->sei;
	*size = vceamf->sei_size;
	return true;*/
}

//TODO win_vceamf_video_info
static void win_vceamf_video_info(void *data, struct video_scale_info *info)
{
	struct win_vceamf *vceamf = static_cast<struct win_vceamf *>(data);
	video_t *video = obs_encoder_video(vceamf->encoder);
	const struct video_output_info *vid_info = video_output_get_info(video);

	vceamf->colorspace = vid_info->colorspace;
	vceamf->range = vid_info->range;

	if (//vid_info->format == VIDEO_FORMAT_I420 ||
	    vid_info->format == VIDEO_FORMAT_NV12)
		return;

	info->format     = VIDEO_FORMAT_NV12;
	info->width      = vid_info->width;
	info->height     = vid_info->height;
	//TODO VIDEO_RANGE_PARTIAL or FULL?
	info->range      = VIDEO_RANGE_PARTIAL;
	info->colorspace = VIDEO_CS_709;

	vceamf->colorspace = info->colorspace;
	vceamf->range = info->range;
	return;
}

void RegisterVCEAMF()
{
	struct obs_encoder_info win_vceamf_encoder = {};
	win_vceamf_encoder.id = "vceamf";
	win_vceamf_encoder.type = OBS_ENCODER_VIDEO;
	win_vceamf_encoder.codec = "h264";
	win_vceamf_encoder.get_name = win_vceamf_getname;
	win_vceamf_encoder.create = win_vceamf_create;
	win_vceamf_encoder.destroy = win_vceamf_destroy;
	win_vceamf_encoder.encode = win_vceamf_encode;
	win_vceamf_encoder.update = win_vceamf_update;
	win_vceamf_encoder.get_properties = win_vceamf_props;
	win_vceamf_encoder.get_defaults = win_vceamf_defaults;
	win_vceamf_encoder.get_extra_data = win_vceamf_extra_data;
	win_vceamf_encoder.get_sei_data = win_vceamf_sei;
	win_vceamf_encoder.get_video_info = win_vceamf_video_info;

	obs_register_encoder(&win_vceamf_encoder);
}