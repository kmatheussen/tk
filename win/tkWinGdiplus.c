/*
 * tkWinGdiplus.c --
 *
 *	Antialiased canvas line drawing for the Windows port, using GDI+.
 *
 *	The Win32 GDI has no antialiased line primitive.  macOS gets
 *	antialiasing "for free" because its XDrawLines implementation uses
 *	CoreGraphics; the Windows port uses GDI's Bresenham rasterizer.  This
 *	file provides an optional path for canvas items that renders their
 *	strokes and fills with GDI+ on top of the canvas pixmap.  The
 *	original GDI drawing remains the fallback for features GDI+ is not
 *	used for (dashes, stipple) and when gdiplus.dll cannot be loaded.
 *
 *	GDI+ is loaded dynamically, like the Direct2D/DirectWrite colour-emoji
 *	path in tkWinColorEmoji.c, so Tk keeps working on systems where
 *	gdiplus.dll is unavailable and no additional import library is needed.
 *
 * Copyright © 2026 The Tk contributors.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWinInt.h"
#include "tkWinGdiplus.h"

/*
 * Runtime controls, mirrored from the macOS ::tk::mac::CGAntialiasLimit
 * interface and the X11 ::tk::x11::canvasAntialias controls:
 *
 *	::tk::win::canvasAntialias	boolean, default 1
 *	::tk::win::canvasAntialiasLimit	integer, default 0
 *
 * A line narrower than the limit is drawn with XDrawLines, as on macOS.
 */

static int gdiplusCanvasAntialias = 1;
static int gdiplusCanvasAntialiasLimit = 0;

/*
 * The dynamically loaded GDI+ flat entry points.  module is NULL when the
 * DLL could not be loaded (remembered after the first attempt), and the
 * function pointers are resolved once.
 */

typedef GpStatus (WINAPI *GdiplusStartupProc)(ULONG_PTR *,
	const GdiplusStartupInput *, GdiplusStartupOutput *);
typedef void (WINAPI *GdiplusShutdownProc)(ULONG_PTR);
typedef GpStatus (WINAPI *GdipCreateFromHDCProc)(HDC, GpGraphics **);
typedef GpStatus (WINAPI *GdipDeleteGraphicsProc)(GpGraphics *);
typedef GpStatus (WINAPI *GdipFlushProc)(GpGraphics *, FlushIntention);
typedef GpStatus (WINAPI *GdipSetSmoothingModeProc)(GpGraphics *,
	SmoothingMode);
typedef GpStatus (WINAPI *GdipSetPixelOffsetModeProc)(GpGraphics *,
	PixelOffsetMode);
typedef GpStatus (WINAPI *GdipCreatePen1Proc)(ARGB, REAL, GpUnit, GpPen **);
typedef GpStatus (WINAPI *GdipDeletePenProc)(GpPen *);
typedef GpStatus (WINAPI *GdipSetPenStartCapProc)(GpPen *, GpLineCap);
typedef GpStatus (WINAPI *GdipSetPenEndCapProc)(GpPen *, GpLineCap);
typedef GpStatus (WINAPI *GdipSetPenLineJoinProc)(GpPen *, GpLineJoin);
typedef GpStatus (WINAPI *GdipCreateSolidFillProc)(ARGB, GpSolidFill **);
typedef GpStatus (WINAPI *GdipDeleteBrushProc)(GpBrush *);
typedef GpStatus (WINAPI *GdipCreatePathProc)(GpFillMode, GpPath **);
typedef GpStatus (WINAPI *GdipDeletePathProc)(GpPath *);
typedef GpStatus (WINAPI *GdipAddPathLineProc)(GpPath *, REAL, REAL, REAL,
	REAL);
typedef GpStatus (WINAPI *GdipAddPathArcProc)(GpPath *, REAL, REAL, REAL,
	REAL, REAL, REAL);
typedef GpStatus (WINAPI *GdipAddPathPieProc)(GpPath *, REAL, REAL, REAL,
	REAL, REAL, REAL);
typedef GpStatus (WINAPI *GdipAddPathEllipseProc)(GpPath *, REAL, REAL, REAL,
	REAL);
typedef GpStatus (WINAPI *GdipClosePathFigureProc)(GpPath *);
typedef GpStatus (WINAPI *GdipDrawPathProc)(GpGraphics *, GpPen *, GpPath *);
typedef GpStatus (WINAPI *GdipFillPathProc)(GpGraphics *, GpBrush *, GpPath *);

typedef struct GdiplusApi {
    HMODULE module;
    ULONG_PTR token;
    GdiplusStartupProc startup;
    GdiplusShutdownProc shutdown;
    GdipCreateFromHDCProc createFromHDC;
    GdipDeleteGraphicsProc deleteGraphics;
    GdipFlushProc flush;
    GdipSetSmoothingModeProc setSmoothingMode;
    GdipSetPixelOffsetModeProc setPixelOffsetMode;
    GdipCreatePen1Proc createPen1;
    GdipDeletePenProc deletePen;
    GdipSetPenStartCapProc setPenStartCap;
    GdipSetPenEndCapProc setPenEndCap;
    GdipSetPenLineJoinProc setPenLineJoin;
    GdipCreateSolidFillProc createSolidFill;
    GdipDeleteBrushProc deleteBrush;
    GdipCreatePathProc createPath;
    GdipDeletePathProc deletePath;
    GdipAddPathLineProc addPathLine;
    GdipAddPathArcProc addPathArc;
    GdipAddPathPieProc addPathPie;
    GdipAddPathEllipseProc addPathEllipse;
    GdipClosePathFigureProc closePathFigure;
    GdipDrawPathProc drawPath;
    GdipFillPathProc fillPath;
} GdiplusApi;

static GdiplusApi gdiplusApi;
static int gdiplusLoaded = 0;	/* -1 = unavailable, 0 = unknown, 1 = loaded. */

/*
 *----------------------------------------------------------------------
 *
 * GdiplusExitHandler --
 *
 *	Shut GDI+ down at process exit.
 *
 *----------------------------------------------------------------------
 */

static void
GdiplusExitHandler(
    TCL_UNUSED(void *))	    /* Not used */
{
    if (gdiplusApi.token != 0) {
	gdiplusApi.shutdown(gdiplusApi.token);
	gdiplusApi.token = 0;
    }
    /* The module handle is left loaded for the lifetime of the process. */
}

/*
 *----------------------------------------------------------------------
 *
 * GdiplusAvailable --
 *
 *	Load gdiplus.dll and resolve the entry points, once.  Returns 1 when
 *	GDI+ is ready for use and 0 otherwise; a failure is remembered so
 *	later calls return at once.
 *
 *----------------------------------------------------------------------
 */

static int
GdiplusAvailable(void)
{
    GdiplusStartupInput input;
    GdiplusStartupOutput output;
    GdiplusApi *api = &gdiplusApi;

    if (gdiplusLoaded != 0) {
	return (gdiplusLoaded > 0);
    }
    gdiplusLoaded = -1;

    if (api->module == NULL) {
	api->module = LoadLibraryW(L"gdiplus.dll");
    }
    if (api->module == NULL) {
	return 0;
    }

#define LOAD(field, name) \
    api->field = (name##Proc) (void *)GetProcAddress(api->module, #name); \
    if (api->field == NULL) { \
	return 0; \
    }

    LOAD(startup,		GdiplusStartup);
    LOAD(shutdown,		GdiplusShutdown);
    LOAD(createFromHDC,		GdipCreateFromHDC);
    LOAD(deleteGraphics,	GdipDeleteGraphics);
    LOAD(flush,		GdipFlush);
    LOAD(setSmoothingMode,	GdipSetSmoothingMode);
    LOAD(setPixelOffsetMode,	GdipSetPixelOffsetMode);
    LOAD(createPen1,		GdipCreatePen1);
    LOAD(deletePen,		GdipDeletePen);
    LOAD(setPenStartCap,	GdipSetPenStartCap);
    LOAD(setPenEndCap,		GdipSetPenEndCap);
    LOAD(setPenLineJoin,	GdipSetPenLineJoin);
    LOAD(createSolidFill,	GdipCreateSolidFill);
    LOAD(deleteBrush,		GdipDeleteBrush);
    LOAD(createPath,		GdipCreatePath);
    LOAD(deletePath,		GdipDeletePath);
    LOAD(addPathLine,		GdipAddPathLine);
    LOAD(addPathArc,		GdipAddPathArc);
    LOAD(addPathPie,		GdipAddPathPie);
    LOAD(addPathEllipse,	GdipAddPathEllipse);
    LOAD(closePathFigure,	GdipClosePathFigure);
    LOAD(drawPath,		GdipDrawPath);
    LOAD(fillPath,		GdipFillPath);

#undef LOAD

    memset(&input, 0, sizeof(input));
    input.GdiplusVersion = 1;
    memset(&output, 0, sizeof(output));
    if (api->startup(&api->token, &input, &output) != 0 || api->token == 0) {
	return 0;
    }
    TkCreateExitHandler(GdiplusExitHandler, NULL);
    gdiplusLoaded = 1;
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * GdiplusCap --
 *
 *	Map a Tk/X11 cap style to the GDI+ line cap.
 *
 *----------------------------------------------------------------------
 */

static GpLineCap
GdiplusCap(
    int capStyle)
{
    switch (capStyle) {
    case CapRound:
	return LineCapRound;
    case CapProjecting:
	return LineCapSquare;
    case CapButt:
    case CapNotLast:
    default:
	return LineCapFlat;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * GdiplusJoin --
 *
 *	Map a Tk/X11 join style to the GDI+ line join.
 *
 *----------------------------------------------------------------------
 */

static GpLineJoin
GdiplusJoin(
    int joinStyle)
{
    switch (joinStyle) {
    case JoinRound:
	return LineJoinRound;
    case JoinBevel:
	return LineJoinBevel;
    case JoinMiter:
    default:
	return LineJoinMiter;
    }
}

/*
 * State for a single antialiased drawing operation: the DC of the drawable,
 * the GDI+ Graphics object created from it and the resolved entry points.
 */

typedef struct WinAASurface {
    TkWinDCState state;
    HDC dc;
    GpGraphics *graphics;
    GdiplusApi *api;
} WinAASurface;

/*
 *----------------------------------------------------------------------
 *
 * BeginAASurface --
 *
 *	Prepare a drawable for antialiased drawing.  Returns 1 on success,
 *	in which case EndAASurface() must be called when done.
 *
 *----------------------------------------------------------------------
 */

static int
BeginAASurface(
    Tk_Canvas canvas,		/* Canvas containing the item. */
    Display *display,		/* Display to draw on. */
    Drawable drawable,		/* Canvas pixmap. */
    WinAASurface *surface)	/* Filled with the drawing state. */
{
    GdiplusApi *api = &gdiplusApi;

    if (!gdiplusCanvasAntialias || canvas == NULL || drawable == None) {
	return 0;
    }
    if (!GdiplusAvailable()) {
	return 0;
    }

    surface->api = api;
    surface->dc = TkWinGetDrawableDC(display, drawable, &surface->state);
    if (api->createFromHDC(surface->dc, &surface->graphics) != 0) {
	TkWinReleaseDrawableDC(drawable, surface->dc, &surface->state);
	return 0;
    }

    /*
     * Antialias, and use Cairo's pixel-centre convention (pixel centres
     * halfway between integer coordinates) so that integer coordinates
     * cover exactly the same pixels as the GDI path.  Odd-width strokes are
     * then offset by half a pixel by TkCanvasPrepareLineAA() to keep 1 px
     * horizontal and vertical lines crisp.
     */

    api->setSmoothingMode(surface->graphics, SmoothingModeAntiAlias);
    api->setPixelOffsetMode(surface->graphics, PixelOffsetModeHalf);
    return 1;
}

/*
 *----------------------------------------------------------------------
 *
 * EndAASurface --
 *
 *	Flush the drawing, release the GDI+ objects and the drawable's DC.
 *	GDI+ and GDI output to the same DC must be synchronized, so the
 *	flush happens before the DC is released for the GDI copy that puts
 *	the canvas pixmap on the screen.
 *
 *----------------------------------------------------------------------
 */

static void
EndAASurface(
    Drawable drawable,
    WinAASurface *surface)
{
    surface->api->flush(surface->graphics, FlushIntentionSync);
    surface->api->deleteGraphics(surface->graphics);
    TkWinReleaseDrawableDC(drawable, surface->dc, &surface->state);
}

/*
 *----------------------------------------------------------------------
 *
 * ColorToARGB --
 *
 *	Convert an XColor to a GDI+ ARGB value.
 *
 *----------------------------------------------------------------------
 */

static ARGB
ColorToARGB(
    XColor *color)
{
    return 0xFF000000
	    | ((color->red >> 8) << 16)
	    | ((color->green >> 8) << 8)
	    | (color->blue >> 8);
}

/*
 *----------------------------------------------------------------------
 *
 * GdiplusAngles --
 *
 *	Convert an X11 start angle and extent (1/64 degrees, positive
 *	angles counter-clockwise on screen) to GDI+ degrees (positive angles
 *	clockwise).  A positive sweep is always produced, so the arc is
 *	traversed from the other end when the X11 extent is positive.
 *
 *----------------------------------------------------------------------
 */

static void
GdiplusAngles(
    int start,
    int extent,
    REAL *startPtr,
    REAL *sweepPtr)
{
    double s = start / 64.0, e = extent / 64.0;

    if (e >= 0.0) {
	*startPtr = (REAL) -(s + e);
	*sweepPtr = (REAL) e;
    } else {
	*startPtr = (REAL) -s;
	*sweepPtr = (REAL) -e;
    }
}

/*
 *----------------------------------------------------------------------
 *
 * TkWinDrawLinesAA --
 *
 *	Draw a solid polyline with GDI+.  Returns true if the line was drawn
 *	here; false means the caller must use XDrawLines.
 *
 *----------------------------------------------------------------------
 */

bool
TkWinDrawLinesAA(
    Tk_Canvas canvas,		/* Canvas containing the item. */
    Display *display,		/* Display to draw on. */
    Drawable drawable,		/* Canvas pixmap. */
    XPoint *points,		/* Polyline vertices. */
    Tcl_Size npoints,		/* Number of vertices. */
    double width,		/* Requested line width. */
    XColor *color,		/* Line color. */
    int capStyle,		/* Tk/X11 cap style. */
    int joinStyle,		/* Tk/X11 join style. */
    Tk_Dash *dash,		/* Dash pattern, or NULL. */
    Pixmap stipple)		/* Stipple pattern, or None. */
{
    WinAASurface surface;
    GpPen *pen;
    GpPath *path;
    GpLineCap lineCap;
    GpLineJoin lineJoin;
    int intWidth;
    int closed, pathOk, drawn;
    double offset;
    Tcl_Size i, last;

    /*
     * Dashed and stippled lines, and lines below the antialias limit, are
     * left to the GC path, which the canvas code has already configured.
     */

    if (color == NULL || npoints < 2) {
	return false;
    }
    if (!TkCanvasPrepareLineAA(width, dash, stipple,
	    gdiplusCanvasAntialiasLimit, &intWidth, &offset)) {
	return false;
    }
    if (!BeginAASurface(canvas, display, drawable, &surface)) {
	return false;
    }

    if (surface.api->createPen1(ColorToARGB(color), (REAL) intWidth,
	    UnitPixel, &pen) != 0) {
	EndAASurface(drawable, &surface);
	return false;
    }
    lineCap = GdiplusCap(capStyle);
    lineJoin = GdiplusJoin(joinStyle);
    surface.api->setPenStartCap(pen, lineCap);
    surface.api->setPenEndCap(pen, lineCap);
    surface.api->setPenLineJoin(pen, lineJoin);

    if (surface.api->createPath(FillModeAlternate, &path) != 0) {
	surface.api->deletePen(pen);
	EndAASurface(drawable, &surface);
	return false;
    }

    /*
     * Build the polyline as a path so the line join is used at every
     * interior vertex.  Match the macOS behaviour for closed polylines:
     * close the figure so the join at the first vertex is stroked as well.
     */

    last = npoints - 1;
    pathOk = 1;
    for (i = 0; i < last && pathOk; i++) {
	pathOk = (surface.api->addPathLine(path,
		(REAL)(points[i].x + offset), (REAL)(points[i].y + offset),
		(REAL)(points[i+1].x + offset),
		(REAL)(points[i+1].y + offset)) == 0);
    }
    closed = (points[0].x == points[last].x && points[0].y == points[last].y);
    if (pathOk && closed) {
	pathOk = (surface.api->closePathFigure(path) == 0);
    }
    drawn = (pathOk && surface.api->drawPath(surface.graphics, pen, path) == 0);

    surface.api->deletePath(path);
    surface.api->deletePen(pen);
    EndAASurface(drawable, &surface);
    return drawn;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWinFillPolygonAA --
 *
 *	Fill a closed polygon with GDI+.  Returns true if the polygon was
 *	drawn here; false means the caller must use the GC path.
 *
 *----------------------------------------------------------------------
 */

bool
TkWinFillPolygonAA(
    Tk_Canvas canvas,		/* Canvas containing the item. */
    Display *display,		/* Display to draw on. */
    Drawable drawable,		/* Canvas pixmap. */
    XPoint *points,		/* Polygon vertices. */
    Tcl_Size npoints,		/* Number of vertices. */
    XColor *color)		/* Fill color. */
{
    WinAASurface surface;
    GpPath *path;
    GpSolidFill *brush;
    Tcl_Size i;
    int pathOk, filled;

    if (color == NULL || npoints < 3) {
	return false;
    }
    if (!BeginAASurface(canvas, display, drawable, &surface)) {
	return false;
    }

    if (surface.api->createPath(FillModeAlternate, &path) != 0) {
	EndAASurface(drawable, &surface);
	return false;
    }
    pathOk = 1;
    for (i = 0; i < npoints && pathOk; i++) {
	Tcl_Size j = (i + 1 == npoints) ? 0 : i + 1;

	pathOk = (surface.api->addPathLine(path, (REAL) points[i].x,
		(REAL) points[i].y, (REAL) points[j].x,
		(REAL) points[j].y) == 0);
    }
    if (pathOk) {
	pathOk = (surface.api->closePathFigure(path) == 0);
    }
    filled = 0;
    if (pathOk && surface.api->createSolidFill(ColorToARGB(color),
	    &brush) == 0) {
	filled = (surface.api->fillPath(surface.graphics, brush,
		path) == 0);
	surface.api->deleteBrush(brush);
    }

    surface.api->deletePath(path);
    EndAASurface(drawable, &surface);
    return filled;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWinDrawArcAA --
 *
 *	Draw the path of an arc, oval, chord or pie slice with GDI+,
 *	filling and/or stroking it as requested.  The start and extent are
 *	in 1/64 degrees (X11 convention).  Returns true if the shape was
 *	drawn here; false means the caller must use the GC path.
 *
 *----------------------------------------------------------------------
 */

bool
TkWinDrawArcAA(
    Tk_Canvas canvas,		/* Canvas containing the item. */
    Display *display,		/* Display to draw on. */
    Drawable drawable,		/* Canvas pixmap. */
    int x1,			/* Left of bounding box. */
    int y1,			/* Top of bounding box. */
    int x2,			/* Right of bounding box. */
    int y2,			/* Bottom of bounding box. */
    int start,			/* Start angle, in 1/64 degrees. */
    int extent,			/* Angular extent, in 1/64 degrees. */
    int style,			/* TK_CANVAS_*_STYLE. */
    XColor *fillColor,		/* Fill color, or NULL for no fill. */
    XColor *outlineColor,	/* Outline color, or NULL for no outline. */
    double width,		/* Outline width. */
    Pixmap stipple)		/* Stipple pattern, or None. */
{
    WinAASurface surface;
    GpPath *path;
    GpPen *pen;
    GpSolidFill *brush;
    REAL bx = (REAL) x1, by = (REAL) y1;
    REAL bw = (REAL) (x2 - x1), bh = (REAL) (y2 - y1);
    REAL startAngle = 0.0, sweepAngle = 0.0;
    double unusedOffset;
    int intWidth = 1, pathOk, filled, drawn;

    if (stipple != None || (fillColor == NULL && outlineColor == NULL)) {
	return false;
    }
    if (extent == 0 && style != TK_CANVAS_OVAL_STYLE) {
	return false;
    }
    if (bw < 1) {
	bw = 1;
    }
    if (bh < 1) {
	bh = 1;
    }
    if (outlineColor != NULL) {
	/*
	 * A curved outline has no useful half-pixel phase, so only the width
	 * and the antialias limit are taken from the shared policy.
	 */

	if (!TkCanvasPrepareLineAA(width, NULL, None,
		gdiplusCanvasAntialiasLimit, &intWidth, &unusedOffset)) {
	    return false;
	}
    }
    if (!BeginAASurface(canvas, display, drawable, &surface)) {
	return false;
    }

    if (surface.api->createPath(FillModeAlternate, &path) != 0) {
	EndAASurface(drawable, &surface);
	return false;
    }

    if (style == TK_CANVAS_OVAL_STYLE) {
	pathOk = (surface.api->addPathEllipse(path, bx, by, bw, bh) == 0);
    } else {
	GdiplusAngles(start, extent, &startAngle, &sweepAngle);
	if (style == TK_CANVAS_PIE_STYLE) {
	    pathOk = (surface.api->addPathPie(path, bx, by, bw, bh,
		    startAngle, sweepAngle) == 0);
	} else {
	    pathOk = (surface.api->addPathArc(path, bx, by, bw, bh,
		    startAngle, sweepAngle) == 0);
	    if (pathOk && style == TK_CANVAS_CHORD_STYLE) {
		pathOk = (surface.api->closePathFigure(path) == 0);
	    }
	}
    }

    filled = 1;
    if (pathOk && fillColor != NULL) {
	filled = 0;
	if (surface.api->createSolidFill(ColorToARGB(fillColor),
		&brush) == 0) {
	    filled = (surface.api->fillPath(surface.graphics, brush,
		    path) == 0);
	    surface.api->deleteBrush(brush);
	}
    }
    drawn = 1;
    if (pathOk && outlineColor != NULL) {
	drawn = 0;
	if (surface.api->createPen1(ColorToARGB(outlineColor),
		(REAL) intWidth, UnitPixel, &pen) == 0) {
	    surface.api->setPenStartCap(pen, LineCapFlat);
	    surface.api->setPenEndCap(pen, LineCapFlat);
	    surface.api->setPenLineJoin(pen, LineJoinMiter);
	    drawn = (surface.api->drawPath(surface.graphics, pen,
		    path) == 0);
	    surface.api->deletePen(pen);
	}
    }

    surface.api->deletePath(path);
    EndAASurface(drawable, &surface);
    return pathOk && filled && drawn;
}

/*
 *----------------------------------------------------------------------
 *
 * TkWinInitGdiplusDrawing --
 *
 *	Create the ::tk::win namespace and link the runtime controls.
 *
 *----------------------------------------------------------------------
 */

void
TkWinInitGdiplusDrawing(
    Tcl_Interp *interp)
{
    TkCanvasInitAntialiasVars(interp, "::tk::win", &gdiplusCanvasAntialias,
	    &gdiplusCanvasAntialiasLimit);

    /*
     * Start GDI+ now, on the thread running TkpInit(), rather than lazily on
     * the first line that needs to be drawn.
     */

    GdiplusAvailable();
}
