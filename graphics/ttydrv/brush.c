/*
 * TTY DC brush
 *
 * Copyright 1999 Patrik Stridvall
 */

#include "brush.h"
#include "dc.h"
#include "debugtools.h"
#include "ttydrv.h"

DEFAULT_DEBUG_CHANNEL(ttydrv)

/***********************************************************************
 *		TTYDRV_DC_BRUSH_SelectObject
 */
HBRUSH TTYDRV_DC_BRUSH_SelectObject(DC *dc, HBRUSH hbrush, BRUSHOBJ *brush)
{
  HBRUSH hPreviousBrush;

  TRACE("(%p, 0x%04x, %p)\n", dc, hbrush, brush);

  hPreviousBrush = dc->w.hBrush;
  dc->w.hBrush = hbrush;

  return hPreviousBrush;
}
