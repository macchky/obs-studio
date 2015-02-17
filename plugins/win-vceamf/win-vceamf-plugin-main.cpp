#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-vceamf", "en-US")

void RegisterVCEAMF();

bool obs_module_load(void)
{
	RegisterVCEAMF();
	return true;
}
