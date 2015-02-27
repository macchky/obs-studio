#include <obs-module.h>
#include <util/platform.h>

void CopyNV12(struct encoder_frame *frame, uint8_t *dst, uint32_t width,
	uint32_t height, uint32_t dstHPitch, uint32_t dstVPitch);