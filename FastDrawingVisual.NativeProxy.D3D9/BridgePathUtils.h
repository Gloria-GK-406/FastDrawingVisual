#pragma once

#include <stddef.h>

#include <d3dx9.h>

bool ResolveShaderCsoPathA(const char *fileName, char *out, size_t outCount);
bool ReadFileToBufferA(const char *path, ID3DXBuffer **outBuffer);
