#include "conversion.hpp"
#define MIN(a,b) ((a) < (b) ? (a) : (b))

void CopyNV12(struct encoder_frame *frame, uint8_t *dst, uint32_t width,
	uint32_t height, uint32_t dstHPitch, uint32_t dstVPitch)
{
	uint32_t y_size = dstHPitch * dstVPitch;
	if (frame->linesize[0] == dstHPitch)
	{
		memcpy(dst, frame->data[0], frame->linesize[0] * height);
		memcpy(dst + y_size, frame->data[1], frame->linesize[1] * height / 2);
	}
	else
	{
		uint8_t *tmp = dst;
		uint8_t *src = frame->data[0];
		size_t pitch = MIN(dstHPitch, frame->linesize[0]);
		for (int y = 0; y < height; y++)
		{
			memcpy(tmp, src, pitch);
			tmp += dstHPitch;
			src += frame->linesize[0];
		}

		tmp = dst + y_size;
		src = frame->data[1];
		pitch = MIN(dstHPitch, frame->linesize[1]);
		for (int y = 0; y < height / 2; y++)
		{
			memcpy(tmp, src, pitch);
			tmp += dstHPitch;
			src += frame->linesize[1];
		}
	}
}