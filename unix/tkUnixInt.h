/*
 * tkUnixInt.h --
 *
 *	This file contains declarations that are shared among the
 *	UNIX-specific parts of Tk but aren't used by the rest of Tk.
 *
 * Copyright © 1995-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#ifndef _TKUNIXINT
#define _TKUNIXINT

#ifndef _TKINT
#include "tkInt.h"
#endif

#ifdef HAVE_BIDI
#  define TK_DRAW_IN_CONTEXT 1
#  define TK_LAYOUT_WITH_BASE_CHUNKS 1
#endif

/*
 * Prototypes for procedures that are referenced in files other than the ones
 * they're defined in.
 */

#include "tkIntPlatDecls.h"  /* IWYU pragma: export */

MODULE_SCOPE  int       Tktray_Init (Tcl_Interp* interp);
MODULE_SCOPE  int       SysNotify_Init (Tcl_Interp* interp);
MODULE_SCOPE  int       Cups_Init (Tcl_Interp* interp);
MODULE_SCOPE  int       TkAtkAccessibility_Init (Tcl_Interp *interp) ;

#ifdef HAVE_CAIRO
MODULE_SCOPE bool	TkUnixDrawLinesAA(Tk_Canvas canvas, Display *display,
			    Drawable drawable, XPoint *points, Tcl_Size npoints,
			    double width, XColor *color, int capStyle,
			    int joinStyle, Tk_Dash *dash, Pixmap stipple);
MODULE_SCOPE bool	TkUnixFillPolygonAA(Tk_Canvas canvas, Display *display,
			    Drawable drawable, XPoint *points, Tcl_Size npoints,
			    XColor *color);
MODULE_SCOPE bool	TkUnixDrawArcAA(Tk_Canvas canvas, Display *display,
			    Drawable drawable, int x1, int y1, int x2, int y2,
			    int start, int extent, int style, XColor *fillColor,
			    XColor *outlineColor, double width, Pixmap stipple);
MODULE_SCOPE void	TkUnixInitCairoDrawing(Tcl_Interp *interp);
MODULE_SCOPE void	TkUnixCairoDrawingEnd(void);
#endif

#endif /* _TKUNIXINT */


/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
