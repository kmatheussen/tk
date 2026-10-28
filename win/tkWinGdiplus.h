/*
 * tkWinGdiplus.h --
 *
 *	C declarations of the subset of the GDI+ flat API used by
 *	tkWinGdiplus.c for antialiased canvas line drawing.  The public
 *	Gdiplus headers of the Windows SDK declare the class API for C++
 *	only; the flat API (GdiplusFlat.h) is C, but its availability differs
 *	between the Microsoft and MinGW-w64 toolchains.  This private copy
 *	gives both toolchains the same declarations, restricted to the
 *	functions tkWinGdiplus.c actually calls.
 *
 *	Reference: the GdiplusFlat.h header shipped with the Windows SDK and
 *	MinGW-w64.  Not to be included together with gdiplus.h or
 *	GdiplusFlat.h.
 *
 * Copyright © 2026 The Tk contributors.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKWINGDIPLUS
#define _TKWINGDIPLUS

#if defined(_GDIPLUS_H) || defined(_GDIPLUSFLAT_H)
#error "tkWinGdiplus.h replaces gdiplus.h and GdiplusFlat.h"
#endif

#include <windows.h>

/*
 * The GDI+ objects are opaque handles in the flat API.
 */

typedef void GpGraphics;
typedef void GpPen;
typedef void GpPath;
typedef void GpBrush;
typedef void GpSolidFill;

typedef DWORD ARGB;
typedef FLOAT REAL;
typedef INT GpStatus;

/*
 * Only the enumerators that tkWinGdiplus.c uses, with their SDK values.
 */

typedef enum {
    SmoothingModeAntiAlias = 4
} SmoothingMode;

typedef enum {
    PixelOffsetModeHalf = 4
} PixelOffsetMode;

typedef enum {
    UnitPixel = 2
} GpUnit;

typedef enum {
    LineCapFlat = 0,
    LineCapSquare = 1,
    LineCapRound = 2
} GpLineCap;

typedef enum {
    LineJoinMiter = 0,
    LineJoinBevel = 1,
    LineJoinRound = 2
} GpLineJoin;

typedef enum {
    FillModeAlternate = 0,
    FillModeWinding = 1
} GpFillMode;

typedef enum {
    FlushIntentionFlush = 0,
    FlushIntentionSync = 1
} FlushIntention;

/*
 * The startup structure layout must match the SDK exactly; only the types of
 * the callback fields differ (function pointer versus void pointer), which
 * keeps the size and alignment identical.
 */

typedef void (WINAPI *GdiplusDebugEventProc)(INT level, char *message);
typedef void (WINAPI *GdiplusNotificationHookProc)(ULONG_PTR *token);
typedef void (WINAPI *GdiplusNotificationUnhookProc)(ULONG_PTR token);

typedef struct {
    UINT32 GdiplusVersion;
    GdiplusDebugEventProc DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef struct {
    GdiplusNotificationHookProc NotificationHook;
    GdiplusNotificationUnhookProc NotificationUnhook;
} GdiplusStartupOutput;

/*
 * The flat API, all exported from gdiplus.dll by name.
 */

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token,
	const GdiplusStartupInput *input, GdiplusStartupOutput *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);

GpStatus WINAPI GdipCreateFromHDC(HDC hdc, GpGraphics **graphics);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *graphics);
GpStatus WINAPI GdipFlush(GpGraphics *graphics, FlushIntention intention);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *graphics,
	SmoothingMode smoothingMode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *graphics,
	PixelOffsetMode pixelOffsetMode);

GpStatus WINAPI GdipCreatePen1(ARGB color, REAL width, GpUnit unit,
	GpPen **pen);
GpStatus WINAPI GdipDeletePen(GpPen *pen);
GpStatus WINAPI GdipSetPenStartCap(GpPen *pen, GpLineCap startCap);
GpStatus WINAPI GdipSetPenEndCap(GpPen *pen, GpLineCap endCap);
GpStatus WINAPI GdipSetPenLineJoin(GpPen *pen, GpLineJoin lineJoin);

GpStatus WINAPI GdipCreateSolidFill(ARGB color, GpSolidFill **brush);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);

GpStatus WINAPI GdipCreatePath(GpFillMode fillMode, GpPath **path);
GpStatus WINAPI GdipDeletePath(GpPath *path);
GpStatus WINAPI GdipAddPathLine(GpPath *path, REAL x1, REAL y1, REAL x2,
	REAL y2);
GpStatus WINAPI GdipAddPathArc(GpPath *path, REAL x, REAL y, REAL width,
	REAL height, REAL startAngle, REAL sweepAngle);
GpStatus WINAPI GdipAddPathPie(GpPath *path, REAL x, REAL y, REAL width,
	REAL height, REAL startAngle, REAL sweepAngle);
GpStatus WINAPI GdipAddPathEllipse(GpPath *path, REAL x, REAL y, REAL width,
	REAL height);
GpStatus WINAPI GdipClosePathFigure(GpPath *path);
GpStatus WINAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path);
GpStatus WINAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush,
	GpPath *path);

#endif /* _TKWINGDIPLUS */
