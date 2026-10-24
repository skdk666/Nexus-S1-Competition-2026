#pragma once
#include "xpt2046_live.h"
void touchLiveUiBegin();
void touchLiveUiUpdate(const xpt2046::LiveResult& result,const char* rx,uint32_t now);
