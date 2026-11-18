#pragma once
#include "common.h"
void GameFixEarly();     // at DLL load: intro skip
void GameFixInstall();   // at the first DirectDrawCreate: everything else
