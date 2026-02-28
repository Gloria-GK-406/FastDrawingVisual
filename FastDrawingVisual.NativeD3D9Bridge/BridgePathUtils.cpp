#include "BridgePathUtils.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

static bool FileExistsA(const char *path) {
  if (!path || path[0] == '\0')
    return false;

  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool JoinPathA(const char *base, const char *leaf, char *out,
                      size_t outCount) {
  if (!base || !leaf || !out || outCount == 0)
    return false;

  size_t baseLen = strlen(base);
  bool needSep = baseLen > 0 && base[baseLen - 1] != '\\' &&
                 base[baseLen - 1] != '/';

  int written = _snprintf_s(out, outCount, _TRUNCATE, "%s%s%s", base,
                            needSep ? "\\" : "", leaf);
  return written > 0;
}

static bool GetModuleDirectoryA(char *out, size_t outCount) {
  if (!out || outCount == 0)
    return false;

  HMODULE module = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&GetModuleDirectoryA), &module) ||
      !module) {
    return false;
  }

  char fullPath[MAX_PATH] = {};
  DWORD len = GetModuleFileNameA(module, fullPath, MAX_PATH);
  if (len == 0 || len >= MAX_PATH)
    return false;

  char *slash = strrchr(fullPath, '\\');
  if (!slash)
    slash = strrchr(fullPath, '/');
  if (!slash)
    return false;

  *slash = '\0';
  return strcpy_s(out, outCount, fullPath) == 0;
}

bool ResolveShaderCsoPathA(const char *fileName, char *out, size_t outCount) {
  if (!fileName || !out || outCount == 0)
    return false;

  char currentDir[MAX_PATH] = {};
  DWORD cwdLen = GetCurrentDirectoryA(_countof(currentDir), currentDir);
  if (cwdLen > 0 && cwdLen < _countof(currentDir)) {
    char candidate[MAX_PATH] = {};
    if (JoinPathA(currentDir, fileName, candidate, _countof(candidate)) &&
        FileExistsA(candidate)) {
      return strcpy_s(out, outCount, candidate) == 0;
    }
  }

  char moduleDir[MAX_PATH] = {};
  if (!GetModuleDirectoryA(moduleDir, _countof(moduleDir)))
    return false;

  char candidate[MAX_PATH] = {};
  if (JoinPathA(moduleDir, fileName, candidate, _countof(candidate)) &&
      FileExistsA(candidate)) {
    return strcpy_s(out, outCount, candidate) == 0;
  }

  return false;
}

bool ReadFileToBufferA(const char *path, ID3DXBuffer **outBuffer) {
  if (!path || !outBuffer)
    return false;

  *outBuffer = nullptr;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE)
    return false;

  DWORD fileSize = GetFileSize(h, nullptr);
  if (fileSize == INVALID_FILE_SIZE || fileSize == 0 || (fileSize % 4) != 0) {
    CloseHandle(h);
    return false;
  }

  ID3DXBuffer *buffer = nullptr;
  if (FAILED(D3DXCreateBuffer(fileSize, &buffer)) || !buffer) {
    CloseHandle(h);
    return false;
  }

  DWORD bytesRead = 0;
  BOOL ok = ReadFile(h, buffer->GetBufferPointer(), fileSize, &bytesRead, nullptr);
  CloseHandle(h);

  if (!ok || bytesRead != fileSize) {
    buffer->Release();
    return false;
  }

  *outBuffer = buffer;
  return true;
}
