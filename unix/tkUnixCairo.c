/*
 * tkUnixCairo.c --
 *
 *	Antialiased canvas line drawing for the X11 port, using Cairo.
 *
 *	The X11 core protocol has no antialiased line primitive.  macOS gets
 *	antialiasing "for free" because its XDrawLines implementation uses
 *	CoreGraphics; the X11 port uses Xlib's Bresenham rasterizer.  This
 *	file provides an optional path for canvas line items that renders the
 *	stroke with Cairo on top of the canvas pixmap.  XDrawLines remains the
 *	fallback for features Cairo does not handle (dashes, stipple) and for
 *	servers/drawables where the Render extension is unavailable.
 *
 * Copyright © 2026 The Tk contributors.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkUnixInt.h"
#include "tkCanvas.h"

#ifdef HAVE_CAIRO

#ifdef HAVE_XRENDER
#include <X11/extensions/Xrender.h>
#endif
#include <cairo.h>
#include <cairo-xlib.h>

/*
 * Runtime controls, mirrored from the macOS ::tk::mac::CGAntialiasLimit
 * interface:
 *
 *	::tk::x11::canvasAntialias	boolean, default 1
 *	::tk::x11::canvasAntialiasLimit	integer, default 0
 *
 * A line narrower than the limit is drawn with XDrawLines, as on macOS.
 */

static int cairoCanvasAntialias = 1;
static int cairoCanvasAntialiasLimit = 0;

/*
 * The canvas draws all of its items into one pixmap per redraw.  Creating a
 * Cairo surface for every line is expensive, so cache the surface and context
 * for the current drawable.  TkCanvasDrawEnd() flushes the cache before the
 * canvas helper frees the pixmap, so the XRender picture never outlives its
 * drawable.
 */

static Display *cairoDisplay = NULL;
static Drawable cairoDrawable = None;
static Visual *cairoVisual = NULL;
static int cairoWidth = 0, cairoHeight = 0;
static cairo_surface_t *cairoSurface = NULL;
static cairo_t *cairoContext = NULL;

static cairo_t *
GetCairoContext(
    Display *display,
    Drawable drawable,
    Visual *visual,
    int width,
    int height)
{
    if (cairoContext != NULL && cairoDisplay == display
	    && cairoDrawable == drawable && cairoVisual == visual
	    && cairoWidth == width && cairoHeight == height
	    && cairo_status(cairoContext) == CAIRO_STATUS_SUCCESS
	    && cairo_surface_status(cairoSurface) == CAIRO_STATUS_SUCCESS) {
	return cairoContext;
    }

    TkUnixCairoDrawingEnd();

    cairoSurface = cairo_xlib_surface_create(display, drawable, visual,
	    width, height);
    if (cairo_surface_status(cairoSurface) != CAIRO_STATUS_SUCCESS) {
	cairo_surface_destroy(cairoSurface);
	cairoSurface = NULL;
	return NULL;
    }
    cairoContext = cairo_create(cairoSurface);
    if (cairo_status(cairoContext) != CAIRO_STATUS_SUCCESS) {
	cairo_destroy(cairoContext);
	cairoContext = NULL;
	cairo_surface_destroy(cairoSurface);
	cairoSurface = NULL;
	return NULL;
    }
    cairoDisplay = display;
    cairoDrawable = drawable;
    cairoVisual = visual;
    cairoWidth = width;
    cairoHeight = height;
    return cairoContext;
}

void
TkUnixCairoDrawingEnd(void)
{
    if (cairoContext != NULL) {
	cairo_destroy(cairoContext);
	cairoContext = NULL;
    }
    if (cairoSurface != NULL) {
	cairo_surface_flush(cairoSurface);
	cairo_surface_destroy(cairoSurface);
	cairoSurface = NULL;
    }
    cairoDisplay = NULL;
    cairoDrawable = None;
    cairoVisual = NULL;
    cairoWidth = cairoHeight = 0;
}

static cairo_line_cap_t
CairoCap(
    int capStyle)
{
    switch (capStyle) {
    case CapRound:
	return CAIRO_LINE_CAP_ROUND;
    case CapProjecting:
	return CAIRO_LINE_CAP_SQUARE;
    case CapButt:
    case CapNotLast:
    default:
	return CAIRO_LINE_CAP_BUTT;
    }
}

static cairo_line_join_t
CairoJoin(
    int joinStyle)
{
    switch (joinStyle) {
    case JoinRound:
	return CAIRO_LINE_JOIN_ROUND;
    case JoinBevel:
	return CAIRO_LINE_JOIN_BEVEL;
    case JoinMiter:
    default:
	return CAIRO_LINE_JOIN_MITER;
    }
}

#ifdef HAVE_XRENDER
/*
 *----------------------------------------------------------------------
 *
 * VisualHasRenderFormat --
 *
 *	Check whether the X server has a Render format for a visual, which
 *	Cairo's Xlib backend needs. The answer only depends on the display
 *	and the visual, so it is cached: this is queried once per line item
 *	per redraw.
 *
 * Results:
 *	True if XRenderFindVisualFormat() knows the visual.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

static bool
VisualHasRenderFormat(
    Display *display,
    Visual *visual)
{
    static Display *cachedDisplay = NULL;
    static Visual *cachedVisual = NULL;
    static bool cachedResult = false;

    if (display != cachedDisplay || visual != cachedVisual) {
	cachedDisplay = display;
	cachedVisual = visual;
	cachedResult = (XRenderFindVisualFormat(display, visual) != NULL);
    }
    return cachedResult;
}
#endif /* HAVE_XRENDER */

/*
 *----------------------------------------------------------------------
 *
 * GetCairoForItem --
 *
 *	Return the cached Cairo context for the drawable the canvas is
 *	currently drawing into, or NULL if the antialiasing path cannot be
 *	used for this item (disabled, unusual visual, no Render extension,
 *	unknown drawable size).
 *
 *----------------------------------------------------------------------
 */

static cairo_t *
GetCairoForItem(
    Tk_Canvas canvas,		/* Canvas being drawn. */
    Display *display,		/* Display to draw on. */
    Drawable drawable)		/* Canvas pixmap. */
{
    Tk_Window tkwin;
    Visual *visual;

    if (!cairoCanvasAntialias || canvas == NULL || drawable == None) {
	return NULL;
    }
    tkwin = Canvas(canvas)->tkwin;
    if (tkwin == NULL || Canvas(canvas)->drawableWidth <= 0
	    || Canvas(canvas)->drawableHeight <= 0) {
	return NULL;
    }

    /*
     * Cairo's Xlib backend needs a visual with a Render format.  Restrict
     * ourselves to the TrueColor/DirectColor visuals where XColor's RGB
     * components are meaningful; other visuals keep the old path.
     */

    visual = Tk_Visual(tkwin);
    if (visual == NULL || (visual->c_class != TrueColor
	    && visual->c_class != DirectColor)) {
	return NULL;
    }
#ifdef HAVE_XRENDER
    if (!VisualHasRenderFormat(display, visual)) {
	return NULL;
    }
#else
    /*
     * Cairo needs an XRender picture format for the destination. Tk can be
     * built with Xrender disabled; keep the old drawing path in that case.
     */
    return NULL;
#endif

    return GetCairoContext(display, drawable, visual,
	    Canvas(canvas)->drawableWidth, Canvas(canvas)->drawableHeight);
}

/*
 *----------------------------------------------------------------------
 *
 * SetSourceColor --
 *
 *	Set the Cairo source to an XColor.
 *
 *----------------------------------------------------------------------
 */

static void
SetSourceColor(
    cairo_t *cr,
    XColor *color)
{
    cairo_set_source_rgba(cr, color->red / 65535.0,
	    color->green / 65535.0, color->blue / 65535.0, 1.0);
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixDrawLinesAA --
 *
 *	Draw a solid polyline with Cairo.  Returns true if the line was
 *	drawn here; false means the caller must use XDrawLines.
 *
 *----------------------------------------------------------------------
 */

bool
TkUnixDrawLinesAA(
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
    cairo_t *cr;
    int intWidth;
    double offset;
    Tcl_Size i;

    /*
     * Dashed and stippled lines, and lines below the antialias limit, are
     * left to the GC path, which the canvas code has already configured.
     */

    if (color == NULL || npoints < 2) {
	return false;
    }
    if (!TkCanvasPrepareLineAA(width, dash, stipple,
	    cairoCanvasAntialiasLimit, &intWidth, &offset)) {
	return false;
    }

    cr = GetCairoForItem(canvas, display, drawable);
    if (cr == NULL) {
	return false;
    }

    cairo_new_path(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    cairo_set_line_width(cr, intWidth);
    cairo_set_line_cap(cr, CairoCap(capStyle));
    cairo_set_line_join(cr, CairoJoin(joinStyle));
    SetSourceColor(cr, color);
    cairo_move_to(cr, points[0].x + offset, points[0].y + offset);
    for (i = 1; i < npoints; i++) {
	cairo_line_to(cr, points[i].x + offset, points[i].y + offset);
    }

    /*
     * Match the macOS behaviour for closed polylines: close the path so the
     * join at the first vertex is stroked as well.
     */

    if (points[0].x == points[npoints-1].x
	    && points[0].y == points[npoints-1].y) {
	cairo_close_path(cr);
    }

    cairo_stroke(cr);
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixFillPolygonAA --
 *
 *	Fill a closed polygon with Cairo.  Returns true if the polygon was
 *	drawn here; false means the caller must use the GC path.
 *
 *----------------------------------------------------------------------
 */

bool
TkUnixFillPolygonAA(
    Tk_Canvas canvas,		/* Canvas containing the item. */
    Display *display,		/* Display to draw on. */
    Drawable drawable,		/* Canvas pixmap. */
    XPoint *points,		/* Polygon vertices. */
    Tcl_Size npoints,		/* Number of vertices. */
    XColor *color)		/* Fill color. */
{
    cairo_t *cr;
    Tcl_Size i;

    if (color == NULL || npoints < 3) {
	return false;
    }
    cr = GetCairoForItem(canvas, display, drawable);
    if (cr == NULL) {
	return false;
    }

    cairo_new_path(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    SetSourceColor(cr, color);
    cairo_move_to(cr, points[0].x, points[0].y);
    for (i = 1; i < npoints; i++) {
	cairo_line_to(cr, points[i].x, points[i].y);
    }
    cairo_close_path(cr);
    cairo_fill(cr);
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixDrawArcAA --
 *
 *	Draw the path of an arc, oval, chord or pie slice with Cairo,
 *	filling and/or stroking it as requested.  The start and extent are
 *	in 1/64 degrees (X11 convention, positive angles counter-clockwise
 *	on screen).  Returns true if the shape was drawn here; false means
 *	the caller must use the GC path.
 *
 *----------------------------------------------------------------------
 */

bool
TkUnixDrawArcAA(
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
    cairo_t *cr;
    double cx, cy, rx, ry, a1, a2, unusedOffset;
    int intWidth = 1;

    if (stipple != None || (fillColor == NULL && outlineColor == NULL)) {
	return false;
    }
    if (extent == 0 && style != TK_CANVAS_OVAL_STYLE) {
	return false;
    }
    if (outlineColor != NULL) {
	/*
	 * A curved outline has no useful half-pixel phase, so only the width
	 * and the antialias limit are taken from the shared policy.
	 */

	if (!TkCanvasPrepareLineAA(width, NULL, None,
		cairoCanvasAntialiasLimit, &intWidth, &unusedOffset)) {
	    return false;
	}
    }
    cr = GetCairoForItem(canvas, display, drawable);
    if (cr == NULL) {
	return false;
    }

    cx = (x1 + x2) / 2.0;
    cy = (y1 + y2) / 2.0;
    rx = (x2 - x1) / 2.0;
    ry = (y2 - y1) / 2.0;
    if (rx < 0.5) {
	rx = 0.5;
    }
    if (ry < 0.5) {
	ry = 0.5;
    }

    /*
     * Cairo's angles increase clockwise on screen (its y axis points down),
     * while X11 angles increase counter-clockwise, so the angles are
     * negated.  Building the ellipse under a scaled transformation and
     * restoring it before filling and stroking keeps the pen circular.
     */

    cairo_new_path(cr);
    cairo_set_antialias(cr, CAIRO_ANTIALIAS_GRAY);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
    if (style == TK_CANVAS_OVAL_STYLE) {
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, rx, ry);
	cairo_arc(cr, 0.0, 0.0, 1.0, 0.0, 2.0 * M_PI);
	cairo_restore(cr);
	cairo_close_path(cr);
    } else {
	a1 = -start * (M_PI / (180.0 * 64.0));
	a2 = -(start + extent) * (M_PI / (180.0 * 64.0));
	cairo_save(cr);
	cairo_translate(cr, cx, cy);
	cairo_scale(cr, rx, ry);
	if (extent >= 0) {
	    cairo_arc_negative(cr, 0.0, 0.0, 1.0, a1, a2);
	} else {
	    cairo_arc(cr, 0.0, 0.0, 1.0, a1, a2);
	}
	cairo_restore(cr);
	if (style == TK_CANVAS_CHORD_STYLE) {
	    cairo_close_path(cr);
	} else if (style == TK_CANVAS_PIE_STYLE) {
	    cairo_line_to(cr, cx, cy);
	    cairo_close_path(cr);
	}
    }

    if (fillColor != NULL) {
	SetSourceColor(cr, fillColor);
	cairo_fill_preserve(cr);
    }
    if (outlineColor != NULL) {
	cairo_set_line_width(cr, intWidth);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
	SetSourceColor(cr, outlineColor);
	cairo_stroke(cr);
    } else {
	cairo_new_path(cr);
    }
    return true;
}

/*
 *----------------------------------------------------------------------
 *
 * TkUnixInitCairoDrawing --
 *
 *	Create the ::tk::x11 namespace and link the runtime controls.
 *
 *----------------------------------------------------------------------
 */

void
TkUnixInitCairoDrawing(
    Tcl_Interp *interp)
{
    TkCanvasInitAntialiasVars(interp, "::tk::x11", &cairoCanvasAntialias,
	    &cairoCanvasAntialiasLimit);
}

#endif /* HAVE_CAIRO */
