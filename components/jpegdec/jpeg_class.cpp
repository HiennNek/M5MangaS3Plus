// C++ wrapper for the JPEGDEC C core.
//
// Upstream's IDF packaging ships the JPEGDEC class *declaration* (in
// JPEGDEC.h) but no C++ translation unit implementing it - the registry
// component only builds the portable C core (JPEGDEC.c), which exports a
// plain C API instead. This file implements the subset of class methods
// used by the app on top of that C API, so call sites can keep the
// original Arduino-style `JPEGDEC` usage unchanged.
#include "JPEGDEC.h"

#include <cstdint>

extern "C"
{
  int JPEG_openRAM(JPEGIMAGE *pJPEG, uint8_t *pData, int iDataSize,
                   JPEG_DRAW_CALLBACK *pfnDraw);
  void JPEG_setPixelType(JPEGIMAGE *pJPEG, int iType);
  int JPEG_decode(JPEGIMAGE *pJPEG, int x, int y, int iOptions);
  void JPEG_close(JPEGIMAGE *pJPEG);
}

int JPEGDEC::openRAM(uint8_t *pData, int iDataSize,
                     JPEG_DRAW_CALLBACK *pfnDraw)
{
  return JPEG_openRAM(&_jpeg, pData, iDataSize, pfnDraw);
}

void JPEGDEC::setPixelType(int iType) { JPEG_setPixelType(&_jpeg, iType); }

void JPEGDEC::setUserPointer(void *p) { _jpeg.pUser = p; }

int JPEGDEC::decode(int x, int y, int iOptions)
{
  return JPEG_decode(&_jpeg, x, y, iOptions);
}

void JPEGDEC::close() { JPEG_close(&_jpeg); }
