// S52CS.c : Conditional Symbologie procedure 3.2 (CS)
//
// Project:  OpENCview

/*
    This file is part of the OpENCview project, a viewer of ENC.
    Copyright (C) 2000-2017 Sylvain Duclos sduclos@users.sourceforge.net

    OpENCview is free software: you can redistribute it and/or modify
    it under the terms of the Lesser GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpENCview is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Lesser GNU General Public License for more details.

    You should have received a copy of the Lesser GNU General Public License
    along with OpENCview.  If not, see <http://www.gnu.org/licenses/>.
*/



// Note: remarks commenting each CS are extracted from pslb03_2.pdf (sec. 12)

#include "S52CS.h"
#include "S52MP.h"      // S52_MP_get/set()
#include "S52utils.h"   // PRINTF(), S52_atof(), S52_atoi(), CCHAR

#include <math.h>       // floor()

#define version "3.2.0" // CS of Plib 3.2 S52 ed ?

#define UNKNOWN_DEPTH -1000.0  // depth of 1km above sea level

// loadCell() - keep ref on S57_geo for further proccessing in CS
typedef struct _localObj {
    GPtrArray *lights_list;  // list of: LIGHTS
    GPtrArray *topmar_list;  // list of: LITFLT, LITVES, BOY???; to find floating platform
    GPtrArray *depcnt_list;  // list of: DEPARE:A, DRGARE:A used by CS(DEPCNT02)
    GPtrArray *udwhaz_list;  // list of: DEPARE:A/L and DRGARE:A used by CS(_UDWHAZ03)
    GPtrArray *depval_list;  // list of geo used by CS(_DEPVAL01) (via OBSTRN04, WRECKS02)
} _localObj;

// Note: seem useless S52 specs -- no effect !?
//GPtrArray *rigid_list; // rigid platform

// size of attributes value list buffer
#define LISTSIZE   16   // list size

#ifdef S52_DEBUG
#include <assert.h>
#define _g_string_new(var,str) (NULL==var) ? g_string_new(str) : (assert(0),(GString*)0);
#else
#define _g_string_new(var,str) g_string_new(str);
#endif  // S52_DEBUG

//#define _g_string_free(var,freeSeg) var=(NULL==var) ? NULL : (g_string_free(var,freeSeg),NULL)  // will fail if freeSeg=FALSE
#define _g_string_free(var,_) var=(NULL==var) ? NULL : (g_string_free(var,TRUE),NULL)             // freeSeg TRUE (str not returned)

static char    *_strpbrk(const char *s, const char *list)
{
    const char *p;
    const char *r;

    if (NULL==s || NULL==list) return NULL;

    for (r=s; *r; r++)
        for (p = list; *p; p++)
            if (*r == *p)
                return (char *)r;

    return NULL;
}

static int      _parseList(const char *str, char *buf)
// Put a string of comma delimited number in an array (buf).
// Return: the number of value in buf.
// Assume: - number < 256,
//         - list size less then LISTSIZE-1 .
// Note: buf is \0 terminated for _strpbrk().
// FIXME: use g_strsplit_set() instead!
{
    int i = 0;

    if (NULL != str && *str != '\0') {
        do {
            if ( i>= LISTSIZE-1) {
                PRINTF("WARNING: value in list lost!!\n");
                break;
            }

            buf[i++] = (unsigned char) S52_atoi(str);

            // skip digit
            while('0'<=*str && *str<='9')
                str++;

        } while(*str++ != '\0');      // skip ',' or exit
    }

    buf[i] = '\0';

    return i;
}

CCHAR    *S52_CS_version(void)
{
    return version;
}

localObj *S52_CS_init (void)
{
    _localObj *local = g_new0(_localObj, 1);
    //_localObj *local = g_try_new0(_localObj, 1);
    if (NULL == local)
        g_assert(0);

    local->lights_list = g_ptr_array_new();
    local->topmar_list = g_ptr_array_new();
    local->depcnt_list = g_ptr_array_new();
    local->udwhaz_list = g_ptr_array_new();
    local->depval_list = g_ptr_array_new();

    return local;
}

localObj *S52_CS_done (_localObj *local)
{
    return_if_null(local);

    // ref only - obj allready deleted
    g_ptr_array_free(local->lights_list, TRUE);
    g_ptr_array_free(local->topmar_list, TRUE);
    g_ptr_array_free(local->depcnt_list, TRUE);
    g_ptr_array_free(local->udwhaz_list, TRUE);
    g_ptr_array_free(local->depval_list, TRUE);

    local->lights_list = NULL;
    local->topmar_list = NULL;
    local->depcnt_list = NULL;
    local->udwhaz_list = NULL;
    local->depval_list = NULL;

    g_free(local);

    return NULL;
}

int       S52_CS_add  (_localObj *local, S57_geo *geo)
// return TRUE
{
    return_if_null(local);
    return_if_null(geo);

    const char *name = S57_getName(geo);

    ///////////////////////////////////////////////
    // for LIGHTS05
    //
    // set floating platform
    if ((0==g_strcmp0(name, "LITFLT")) ||
        (0==g_strcmp0(name, "LITVES")) ||
        (0==strncmp  (name, "BOY", 3)))
    {
        g_ptr_array_add(local->topmar_list, (gpointer) geo);
        return TRUE;
    }

    // Note: seem useless S52 specs -- no effect !?
    // set rigid platform
    //if (0 == g_strcmp0(name, "BCN",    3))
    //    g_ptr_array_add(local->rigid_list, (gpointer) geo);

    // set light object
    if (0 == g_strcmp0(name, "LIGHTS")) {
        // Note: order of S57ID are preserved (ID1 < ID2 < ID3 ..)
        g_ptr_array_add(local->lights_list, (gpointer) geo);
        return TRUE;
    }


    ///////////////////////////////////////////////
    // for DEPCNT02: build ref to group 1 DEPARE:A and DRGARE:A (depcnt_list)
    // Note: the Nassi graph say to loop AREA of DEPARE and DRGARE
    //
    // for _UDWHAZ03 (via OBSTRN04, WRECKS02)
    // build ref to group 1 DEPARE:A/L and DRGARE:A   (udwhaz_list)
    if ((0==g_strcmp0(name, "DEPARE")) ||    // LINE/AREA:
        (0==g_strcmp0(name, "DRGARE"))       // AREA:
       )
    {
        // DEPARE:A/L and DRGARE:A
        g_ptr_array_add(local->udwhaz_list, (gpointer) geo);

        // DEPARE:A and DRGARE:A
        if (S57_AREAS_T == S57_getObjtype(geo)) {
            g_ptr_array_add(local->depcnt_list, (gpointer) geo);
        }
    }

    ///////////////////////////////////////////////
    // for _DEPVAL01 (via OBSTRN04, WRECKS02)
    if ((0==g_strcmp0(name, "DEPARE")) ||    // LINE/AREA:
        (0==g_strcmp0(name, "UNSARE"))       // AREA:
       )
    {
        // DEPARE:A/L and UNSARE:A
        g_ptr_array_add(local->depval_list, (gpointer) geo);
    }

    return TRUE;
}

int       S52_CS_touch(_localObj *local, S57_geo *geo)
// compute witch geo object of this cell "touch" this one (geo)
// return TRUE
{
    // useless - rbin
    //return_if_null(local);
    //return_if_null(geo);

    const char *name = S57_getName(geo);

    ////////////////////////////////////////////
    // floating object
    if (0 == g_strcmp0(name, "TOPMAR")) {
        for (guint i=0; i<local->topmar_list->len; ++i) {
            S57_geo *other = (S57_geo *) g_ptr_array_index(local->topmar_list, i);

            // skip if not at same position
            if (FALSE == S57_cmpGeoExt(geo, other))
                continue;

            if (NULL == S57_getTouchTOPMAR(geo)) {
                S57_setTouchTOPMAR(geo, other);

                // bailout as soon as we got one - this assume that there at most 1 topmar!
                //break;
            } else {
                static int silent = FALSE;
                if (FALSE == silent) {
                    PRINTF("FIXME: more than 1 TOPMAR\n");
                    PRINTF("FIXME: (this msg will not repeat)\n");
                    silent = TRUE;

                    g_assert(0);
                }
            }
        }

        // finish
        return TRUE;
    }

    ////////////////////////////////////////////
    // experimental:
    // check if this buoy has a lights
    if (0 == g_strcmp0(name, "BOYLAT")) {
        for (guint i=0; i<local->lights_list->len; ++i) {

            S57_geo *light = (S57_geo *) g_ptr_array_index(local->lights_list, i);

            // skip if this light is not at buoy's position
            if (FALSE == S57_cmpGeoExt(geo, light))
                continue;

            // debug
            if (NULL != S57_getTouchLIGHTS(light)) {
                static int silent = FALSE;
                if (FALSE == silent) {
                    PRINTF("FIXME: A) more than 1 LIGHT for the same BOYLAT\n");
                    PRINTF("FIXME: (this msg will not repeat)\n");
                    silent = TRUE;
                }
            }

            // reverse chaining
            S57_setTouchLIGHTS(light, geo);

            // bailout as soon as we got one
            break;
        }
        return TRUE;
    }

    ////////////////////////////////////////////
    // LIGHTS05:sector
    // chaine light at same position
    if (0 == g_strcmp0(name, "LIGHTS")) {
        for (guint i=0; i<local->lights_list->len; ++i) {
            S57_geo *candidate = (S57_geo *) g_ptr_array_index(local->lights_list, i);

            // skip if allready processed / same LIGHTS
            if (S57_getS57ID(candidate) <= S57_getS57ID(geo))
                continue;

            // skip if not at same position
            if (FALSE == S57_cmpGeoExt(geo, candidate))
                continue;

            // chaine lights
            if (NULL == S57_getTouchLIGHTS(geo)) {
                S57_setTouchLIGHTS(geo, candidate);

                // bailout as soon as we get one
                break;  // debug - comment this to scan all lights
            } else {
                // parano
                PRINTF("FIXME: chaining prob.: more than 1 LIGHT touch this LIGHT\n");
                g_assert(0);
            }
        }

        // finish
        return  TRUE;
    }

    //////////////////////////////////////////////
    // DEPCNT02
    // DEPCNT:L, DEPARE:L call CS(DEPCNT02)
    // depcnt_list: a set of DEPARE:A and DRGARE:A
    // link to the shallower object that intersec this object
    // Note: S52 has no ordering of depcnt_list, so all candite are checked
    if ((0==g_strcmp0(name, "DEPCNT")) ||                                  // LINE
        (0==g_strcmp0(name, "DEPARE") && S57_LINES_T==S57_getObjtype(geo)) // LINE, AREA (www.s57.com 2017 say AREA only)
       )
    {
        GString  *drvalstr = NULL;
        //double    drvalmin    = 0.0;
        double    drvalmin    = UNKNOWN_DEPTH;

        CCHAR *name = S57_getName(geo);

        // DEPCNT
        if (0 == g_strcmp0(name, "DEPCNT")) {
            //drvalstr = S57_getAttVal(geo, "VALDCO");  // mandatory!
            drvalstr = S57_getAttValALL(geo, "VALDCO");  // mandatory!
        } else {
            // DEPARE
            //drvalstr = S57_getAttVal(geo, "DRVAL1");  // mandatory!
            drvalstr = S57_getAttValALL(geo, "DRVAL1");  // mandatory!
        }

        if (NULL != drvalstr) {
            if (0 != g_strcmp0(drvalstr->str, EMPTY_NUMBER_MARKER)) {
                drvalmin = S52_atof(drvalstr->str);
            }
        } else {
            PRINTF("DEBUG: line DEPCNT/DEPARE:%u has no mandatory depth or (VALDCO/DRVAL1)\n", S57_getS57ID(geo));
            //g_assert(0);
            return TRUE;
        }

        // debug - DEPARE:3653
        if (3653 == S57_getS57ID(geo)) {
            PRINTF("DEBUG: line DEPARE:%u found\n", S57_getS57ID(geo));
            //PRINTF("DEBUG: line DEPCNT:%u found\n", S57_getS57ID(geo));
            //g_assert(0);
            //S57_setHighlight(candidate, TRUE);
        }

        guint   npt;
        double *ppt;
        if (FALSE == S57_getGeoData(geo, 0, &npt, &ppt))
            return FALSE;

        // Note: a line segment is made of edge primtive  (CN - EN - .. - EN - CN)
        // FIXME: find if that make a diff CN/EN
        //if (2 < npt)
        //    ppt += 3;  // take the first EN

        // select the next deeper contour as the safety contour
        // when the contour requested is not in the database
        for (guint i=0; i<local->depcnt_list->len; ++i) {
            S57_geo *candidate = (S57_geo *) g_ptr_array_index(local->depcnt_list, i);

            // skip if it's same S57 object (DEPARE)
            if (S57_getS57ID(geo) == S57_getS57ID(candidate))
                continue;

            if (FALSE == S57_cmpGeoExt(geo, candidate))
                continue;

            if (FALSE == S57_isPtInSet(candidate, ppt[0], ppt[1]))
                continue;

            //
            // link to the line above this geo (DEPARE/DRGARE)
            //

            // DRVAL1 mandatory DEPARE and DRGARE
            GString *can_drval1str = S57_getAttVal(candidate, "DRVAL1");
            if (NULL != can_drval1str) {
                double can_drval1 = S52_atof(can_drval1str->str);

                //* clear default
                if (UNKNOWN_DEPTH == drvalmin) {
                    drvalmin = can_drval1;
                    continue;
                }
                //*/

                // is this area just above (shallower) then this geo
                //if (can_drval1 < drvalmin) {
                // deeper
                if (can_drval1 > drvalmin) {
                    drvalmin = can_drval1;
                    S57_setTouchDEPCNT(geo, candidate);

                    /* debug
                    if (5573 == S57_getS57ID(geo)) {
                        //PRINTF("DEBUG: line DEPCNT:%u found\n", S57_getS57ID(geo));
                        //g_assert(0);
                        S57_setHighlight(candidate, TRUE);
                    }
                    */
                }
            }
        } // for

        return TRUE;
    }

    ////////////////////////////////////////
    // _UDWHAZ03 (via OBSTRN04, WRECKS02)
    // OBSTRN:A/L/P call OBSTRN04
    // UWTROC:P     call OBSTRN04
    // WRECKS:A/P   call WRECKS02
    // and in turn, call _UDWHAZ03 to find out if a DEPARE:A/L and DGRARE:A is deeper than the safety contour
    if ((0==g_strcmp0(name, "OBSTRN")) ||
        (0==g_strcmp0(name, "UWTROC")) ||
        (0==g_strcmp0(name, "WRECKS"))
       )
    {
        guint   npt;
        double *ppt;
        if (FALSE == S57_getGeoData(geo, 0, &npt, &ppt))
            return FALSE;

        // Note: a line segment is made of edge primtive  (CN - EN - .. - EN - CN)
        // FIXME: find if that make a diff CN/EN
        //if (2 < npt)
        //    ppt += 3;  // take the first EN

        // find the deepest group 1 under this geo
        //double depth_max = 0.0;
        double depth_max = UNKNOWN_DEPTH;
        for (guint i=0; i<local->udwhaz_list->len; ++i) {
            // list of DEPARE:L/A and DRGARE:A
            S57_geo *candidate = (S57_geo *) g_ptr_array_index(local->udwhaz_list, i);

            // skip if not overlapping
            if (FALSE == S57_cmpGeoExt(geo, candidate))
                continue;

            //
            // is geo touching this candidate?
            //

            // geo point (IsolatedNode)
            if (S57_POINT_T == S57_getObjtype(geo)) {
                // candidate line
                if (S57_LINES_T == S57_getObjtype(candidate)) {
                    if (FALSE == S57_isPtOnLine(candidate, ppt[0], ppt[1]))
                        continue;
                } else {
                    // candidate area
                    if (FALSE == S57_isPtInArea(candidate, ppt[0], ppt[1])) {
                        continue;
                    }
                }
            } else {
                // geo:A/L, candidate:A/L
                if (FALSE == S57_isPtInSet(candidate, ppt[0], ppt[1]))
                    continue;
            }

            //
            // geo do touch this candidate
            //

            // debug - then link to it as default
            //S57_setTouchUDWHAZ(geo, candidate);

            //get depth_max
            if (S57_LINES_T == S57_getObjtype(candidate)) {
                // DEPARE:L use DRVAL2 (not in UDWHAZ04)
                GString *drval2str = S57_getAttVal(candidate, "DRVAL2");
                if (NULL != drval2str) {
                    double drval2 = S52_atof(drval2str->str);
                    if (drval2 > depth_max) {
                        depth_max = drval2;
                        S57_setTouchUDWHAZ(geo, candidate);
                    }
                }
            } else {
                // DEPARE:A and DRGARE:A use DRVAL1
                // If there is no explicit value, go to the next object because we
                // consider, empty DRVAL1 is always less SAFETY_CONTOUR
                GString *drval1str = S57_getAttVal(candidate, "DRVAL1");
                if (NULL != drval1str) {
                    double drval1 = S52_atof(drval1str->str);
                    if (drval1 > depth_max) {
                        depth_max = drval1;
                        S57_setTouchUDWHAZ(geo, candidate);
                    }
                    //else {
                    //    PRINTF("DEBUG: group 1 candidate DRVAL1=%s under this: %s:%c:%i\n", drval1str->str, name, S57_getObjtype(geo), S57_getS57ID(geo));
                    //}
                }
                //else {
                //    PRINTF("DEBUG: group 1 candidate has no DRVAL1 value under this: %s:%c:%i\n", name, S57_getObjtype(geo), S57_getS57ID(geo));
                //}
            }

        }  // for loop

        // FIXME: how to handle geo sans candidate - no DRVAL1 or DRVAL2 !!
        if (NULL == S57_getTouchUDWHAZ(geo)) {
            PRINTF("DEBUG: no group 1 candidate under this: %s:%c:%i\n", name, S57_getObjtype(geo), S57_getS57ID(geo));
        }
    }

    //////////////////////////////////////
    // _DEPVAL01
    // OBSTRN:A/L/P call OBSTRN04
    // UWTROC:P     call OBSTRN04
    // WRECKS:A/P   call WRECKS02
    // in turn call _DEPVAL01 to find the geo oject of 'least_depht'
    if ((0==g_strcmp0(name, "OBSTRN")) ||
        (0==g_strcmp0(name, "UWTROC")) ||
        (0==g_strcmp0(name, "WRECKS"))
       )
    {
        guint   npt;
        double *ppt;
        if (FALSE == S57_getGeoData(geo, 0, &npt, &ppt))
            return FALSE;

        // Note: a line segment is made of edge primtive  (CN - EN - .. - EN - CN)
        // FIXME: find if that make a diff CN/EN
        if (2 < npt)
            ppt += 3;  // take the first EN

        double least_depth = INFINITY;

        for (guint i=0; i<local->depval_list->len; ++i) {
            S57_geo *candidate = (S57_geo *) g_ptr_array_index(local->depval_list, i);

            // skip if extent not overlapping
            if (FALSE == S57_cmpGeoExt(geo, candidate))
                continue;

            //
            // is geo touching this candidate?
            //

            // geo:P (IsolatedNode)
            if (S57_POINT_T == S57_getObjtype(geo)) {
                // candidate:L
                if (S57_LINES_T == S57_getObjtype(candidate)) {
                    S57_isPtOnLine(candidate, ppt[0], ppt[1]);
                }

                // candidate:A
                if (FALSE ==S57_isPtInArea(candidate, ppt[0], ppt[1])) {
                    continue;
                }

            } else {
                // geo:A/L, candidate:A/L
                if (FALSE == S57_isPtInSet(candidate, ppt[0], ppt[1]))
                    continue;
            }

            //
            // geo do touch this candidate
            //

            // first check if UNSARE
            if (0 == g_strcmp0(S57_getName(candidate), "UNSARE")) {
                S57_setTouchDEPVAL(geo, candidate);

                PRINTF("DEBUG: UNSARE found value under this: %s:%c:%i\n", name, S57_getObjtype(geo), S57_getS57ID(geo));
                g_assert(0);
                break;  // bailout - no need to search further
            }

            S57_geo *crntmin = S57_getTouchDEPVAL(geo);
            if (NULL == crntmin) {
                S57_setTouchDEPVAL(geo, candidate);
            } else {
                GString *drval1str = S57_getAttVal(candidate, "DRVAL1");
                if (NULL == drval1str)
                    continue;

                double drval1 = S52_atof(drval1str->str);
                if (isinf(least_depth)) {
                    least_depth = drval1;
                    S57_setTouchDEPVAL(geo, candidate);
                }

                if (drval1 < least_depth) {
                //if (least_depth < drval1 ) {
                    continue;
                } else {
                    least_depth = drval1;
                    S57_setTouchDEPVAL(geo, candidate);
                }
            }

        }  // for loop

        // FIXME: how to handle geo sans candidate - no DRVAL1 or DRVAL2 !!
        if (NULL == S57_getTouchDEPVAL(geo)) {
            PRINTF("DEBUG: no group 1 candidate under this: %s:%c:%i\n", name, S57_getObjtype(geo), S57_getS57ID(geo));
        }
    }

    return TRUE;
}

static GString *CLRLIN01 (S57_geo *geo)
// Remarks: A clearing line shows a single arrow head at one of its ends. The direction
// of the clearing line must be calculated from its line object in order to rotate
// the arrow head symbol and place it at the correct end. This cannot be
// achieved with a complex linestyle since linestyle symbols cannot be sized
// to the length of the clearing line. Instead a linestyle with a repeating pattern
// of arrow symbols had to be used which does not comply with the required
// symbolization.
{
    GString *clrlin01  = _g_string_new(NULL, ";SY(CLRLIN01);LS(SOLD,1,NINFO)");
    GString *catclrstr = S57_getAttVal(geo, "catclr");

    if (NULL != catclrstr) {

        //if '0' : txt undefined

        // NMT
        if ('1' == *catclrstr->str) {
            g_string_append(clrlin01, ";TX('NMT',2,1,2,'15110',-1,-1,CHBLK,51)");
        }

        // NLT
        if ('2' == *catclrstr->str) {
            g_string_append(clrlin01, ";TX('NLT',2,1,2,'15110',-1,-1,CHBLK,51)");
        }
    }

    return clrlin01;
}

static GString *DATCVR01 (S57_geo *geo)
// Remarks: This conditional symbology procedure describes procedures for:
// - symbolizing the limit of ENC coverage;
// - symbolizing navigational purpose boundaries ("scale boundarie"); and
// - indicating overscale display.
//
// Note that the mandatory meta object M_QUAL:CATQUA is symbolized by the look-up table.
//
// Because the methods adopted by an ECDIS to meet the IMO and IHO requirements
// listed on the next page will depend on the manufacturer's software, and cannot be
// described in terms of a flow chart in the same way as other conditional procedures,
// this procedure is in the form of written notes.
{
    // Note: this CS apply to object M_COVR and M_CSCL

    GString *datcvr01 = _g_string_new(NULL, NULL);

    ///////////////////////
    // 1 - REQUIREMENT
    // (IMO/IHO specs. explenation)

    ///////////////////////
    // 2 - ENC COVERAGE
    //
    // 2.1 - Limit of ENC coverage
    // CSG union of all M_COVR:CATCOV=1
    if (0 == g_strcmp0(S57_getName(geo), "M_COVR")) {
        /* Note: M_COVR/CATCOV=  1 or 2 distiction is now maid in S52.c:_app()
        GString *catcovstr = S57_getAttVal(geo, "CATCOV");
        if ((NULL!=catcovstr) && ('1'==*catcovstr->str)) {
            // M_COVR:CATCOV=1, link to PLib AUX 'm_covr'
            // Note: this rule apply to the final union poly
            // not to individual S57 object
            datcvr01 = g_string_new(";OP(3OD11060);LC(HODATA01)");
        } else {
            // M_COVR:CATCOV=2, link to PLib 'M_COVR'
            // LUPT   40LU00102NILM_COVRA00001SPLAIN_BOUNDARIES
            // LUPT   45LU00357NILM_COVRA00001SSYMBOLIZED_BOUNDARIES
            datcvr01 = g_string_new(";LC(HODATA01)");
        }
        //*/

        //datcvr01 = g_string_new(";LC(HODATA01)");
        datcvr01 = g_string_append(datcvr01, ";LC(HODATA01)");
        return datcvr01;
    }

    // 2.2 - No data areas
    // FIXME: This can be done outside of CS (ie when clearing the screen)
    // FIXME: ";OP(0OD11050);AC(NODATA);AP(NODATA)"
    // FIXME: set geo to cover earth (!)

    //////////////////////
    // 3 - SCALE BOUNDARIES
    //
    // 3.1 - Chart scale boundaties
    // FIXME: use Data Set IDentification field,
    // intended usage (navigational purpose) (DSID,INTU)
    //if (0 == g_strcmp0(S57_getName(geo), "DSID")) {  // not reached
    if (0 == g_strcmp0(S57_getName(geo), "M_COVR")) {
        //GString *intustr = S57_getAttVal(geo, "INTU");
        //g_string_append(datcvr01, ";OP(3OS21030);LS(SOLD,1,CHGRD)");
        // -OR-
        //g_string_append(datcvr01, ";OP(3OS21030);LC(SCLBDYnn)");
        PRINTF("DEBUG: M_COVR found\n");
        g_assert(0);
    }
    // 3.2 - Graphical index of navigational purpose
    // FIXME: draw extent of available SENC in DB (CATALOG)
    // -OR- client job! do libS52.so only render cell?

    //////////////////////
    // 4 - OVERSCALE
    //
    // FIXME: get metadata CSCL of DSPM field
    // FIXME: get object M_CSCL or CSCALE
    // in gdal is named:
    // DSID:DSPM_CSCL (Data Set ID - metadata)
    // M_CSCL:CSCALE
    if (0 == g_strcmp0(S57_getName(geo), "M_CSCL")) {
        PRINTF("FIXME: overscale M_CSCL not computed\n");
        return datcvr01;
    }
    //
    // 4.1 - Overscale indication
    // FIXME: compute, scale = [denominator of the compilation scale] / [denominator of the display scale]
    // FIXME: draw overscale indication (ie TX("X%3.1f",scale))
    //        color SCLBR, display base

    //
    // 4.2 - Ovescale area at a chart scale boundary
    // FIXME: to  put on STANDARD DISPLAY but this object
    // is on DISPLAYBASE in section 2
    //g_string_append(datcvr01, ";OP(3OS21030);AP(OVERSC01)");

    //
    // 4.3 - Larger scale data available
    // FIXME: display indication of better scale available (?)

    // FIXME
    static int silent = FALSE;
    if (FALSE == silent) {
        PRINTF("NOTE: DATCVR01/OVERSCALE not computed\n");
        PRINTF("NOTE: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return datcvr01;
}

static GString *DATCVR02 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(DATCVR02) switch to CS(DATCVR01)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return DATCVR01(geo);
}

static GString *_SEABED01(double drval1, double drval2);
//static GString *_RESCSP01(S57_geo *geo);
static GString *_RESCSP01(GString *restrn01str);
static GString *DEPARE01 (S57_geo *geo)
// Remarks: An object of the class "depth area" is coloured and covered with fill patterns
// according to the mariners selections of shallow contour, safety contour and
// deep contour. This requires a decision making process provided by the sub-procedure
// "SEABED01" which is called by this symbology procedure.
// Objects of the class "dredged area" are handled by this routine as well to
// ensure a consistent symbolization of areas that represent the surface of the
// seabed.
{
    GString *depare01  = NULL;
    //int      objl      = 0;
    //GString *objlstr   = NULL;
    GString *drval1str = S57_getAttVal(geo, "DRVAL1");
    double   drval1    = (NULL == drval1str) ? -1.0        : S52_atof(drval1str->str);
    GString *drval2str = S57_getAttVal(geo, "DRVAL2");
    double   drval2    = (NULL == drval2str) ? drval1+0.01 : S52_atof(drval2str->str);

    // adjuste datum
    drval1 += S52_MP_get(S52_MAR_DATUM_OFFSET);
    drval2 += S52_MP_get(S52_MAR_DATUM_OFFSET);

    depare01 = _SEABED01(drval1, drval2);

    /*
    GString *objlstr = S57_getAttVal(geo, "OBJL");
    int      objl    = (NULL == objlstr) ? 0 : S52_atoi(objlstr->str);

    if (DRGARE == objl) {
    */

    if (0 == g_strcmp0(S57_getName(geo), "DRGARE")) {
        g_string_append(depare01, ";AP(DRGARE01)");
        g_string_append(depare01, ";LS(DASH,1,CHGRF)");

        GString *restrn01str = S57_getAttVal(geo, "RESTRN");
        if (NULL != restrn01str) {
            GString *rescsp01 = _RESCSP01(restrn01str);
            if (NULL != rescsp01) {
                g_string_append(depare01, rescsp01->str);
                _g_string_free(rescsp01, TRUE);
            }
        }
    }

    return depare01;
}

static GString *DEPARE02 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(DEPARE02) --> CS(DEPARE01)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return DEPARE01(geo);
}

static GString *DEPARE03 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(DEPARE03) --> CS(DEPARE01)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return DEPARE01(geo);
}

static gboolean _DEPCNT02_isSafetyCnt(S57_geo *geo)
// true if safety contour else false
{

    /*
    GString *drval1touchstr = NULL;
    S57_geo *geoTouch       = S57_getTouchDEPCNT(geo);
    if (NULL != geoTouch) {
        drval1touchstr = S57_getAttVal(geoTouch, "DRVAL1");
    }

    double drval1touch = (NULL == drval1touchstr) ? 0.0 : S52_atof(drval1touchstr->str);

    if (NULL != drval1touchstr) {
        // adjuste datum
        drval1touch += S52_MP_get(S52_MAR_DATUM_OFFSET);

        if (drval1touch < S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
            safety_contour = TRUE;
        }
    }
    */

    /* DEPCNT:5573, DEPCNT:4153
    //if (5573 == S57_getS57ID(geo)) {
    if (4153 == S57_getS57ID(geo)) {
        PRINTF("DEBUG: line DEPCNT:%u found\n", S57_getS57ID(geo));
    // DEPARE:3653
    //if (3653 == S57_getS57ID(geo)) {
    //    PRINTF("DEBUG: line DEPARE:%u found\n", S57_getS57ID(geo));
        g_assert(0);
    }
    //*/

    gboolean safety_contour = FALSE;

    S57_geo *geoTouch = S57_getTouchDEPCNT(geo);
    if (NULL != geoTouch) {
        GString *drval1touchstr = S57_getAttVal(geoTouch, "DRVAL1");
        if (NULL != drval1touchstr) {
            double drval1touch = S52_atof(drval1touchstr->str);

            // adjuste datum
            drval1touch += S52_MP_get(S52_MAR_DATUM_OFFSET);

            if (drval1touch < S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {

                // Note: not is S52 - fix false positive safety_cnt
                GString *drval2str = S57_getAttVal(geo, "DRVAL2");
                double   drval2    = (NULL == drval2str) ? 0.0 : S52_atof(drval2str->str);

                // adjuste datum
                drval1touch += S52_MP_get(S52_MAR_DATUM_OFFSET);

                // invariant: DRVAL1 <= SC <= DRVAL2 (top of S52 3.2 DEPCNT02 Nassi flow chart)
                if (drval2 >= S52_MP_get(S52_MAR_SAFETY_CONTOUR))
                    safety_contour = TRUE;
            }
        }
        // FIXME: document skip logic branch DRVAL1 not present
    }

    return safety_contour;
}

static GString *_SNDFRM02(S57_geo *geo, double depth_value);  // forward decl

static GString *DEPCNT02 (S57_geo *geo)
// Remarks: An object of the class "depth contour" or "line depth area" is highlighted and must
// be shown under all circumstances if it matches the safety contour depth value
// entered by the mariner (see IMO PS 3.6). But, while the mariner is free to enter any
// safety contour depth value that he thinks is suitable for the safety of his ship, the
// SENC only contains a limited choice of depth contours. This symbology procedure
// determines whether a contour matches the selected safety contour. If the selected
// safety contour does not exist in the data, the procedure will default to the next deeper
// contour. The contour selected is highlighted as the safety contour and put in
// DISPLAYBASE. The procedure also identifies any line segment of the spatial
// component of the object that has a "QUAPOS" value indicating unreliable
// positioning, and symbolizes it with a double dashed line.
//
// Note: Depth contours are not normally labeled. The ECDIS may provide labels, on demand
// only as with other text, or provide the depth value on cursor picking
{
    GString *depcnt02       = NULL;
    gboolean safety_contour = FALSE;  // initialy not a safety contour
    //double   depth_value;           // for depth label (facultative in S-52)

    // first reset original scamin
    S57_setScamin(geo, S57_RESET_SCAMIN);

    // DEPARE (line)
    if (0 == g_strcmp0(S57_getName(geo), "DEPARE")) {  // only DEPARE:L call CS(DEPCNT02)
        // Note: if drval1 not given then set it to 0.0 (ie. LOW WATER LINE as FAIL-SAFE)
        GString *drval1str = S57_getAttVal(geo, "DRVAL1");
        double   drval1    = (NULL == drval1str) ? 0.0    : S52_atof(drval1str->str);
        GString *drval2str = S57_getAttVal(geo, "DRVAL2");
        double   drval2    = (NULL == drval2str) ? drval1 : S52_atof(drval2str->str);

        // adjuste datum
        drval1 += S52_MP_get(S52_MAR_DATUM_OFFSET);
        drval2 += S52_MP_get(S52_MAR_DATUM_OFFSET);

        if (drval1 <= S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
            if (drval2 >= S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
                safety_contour = TRUE;
            }
        } else {
            // collect area DEPARE & DRGARE that touch this line
            safety_contour = _DEPCNT02_isSafetyCnt(geo);
        }

    } else {
        // continuation A (DEPCNT (line)) - only DEPCNT:L call CS(DEPCNT02)
        GString *valdcostr = S57_getAttVal(geo, "VALDCO");
        double   valdco    = (NULL == valdcostr) ? 0.0 : S52_atof(valdcostr->str);

        // adjuste datum
        valdco += S52_MP_get(S52_MAR_DATUM_OFFSET);

       if (valdco == S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
            safety_contour = TRUE;
        } else {
            if (valdco > S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
                // collect area DEPARE & DRGARE that touche this line
                safety_contour = _DEPCNT02_isSafetyCnt(geo);
            }
        }
    }

    // Continuation B
    // ASSUME: HO split lines to preserv different QUAPOS for a given line
    // FIXME: check that the assumtion above is valid!
    GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
    if (NULL != quaposstr) {
        int quapos = S52_atoi(quaposstr->str);
        if ( 2 <= quapos && quapos < 10) {
            if (TRUE == safety_contour) {
                depcnt02 = _g_string_new(depcnt02, ";LS(DASH,2,DEPSC)");
            } else {
                depcnt02 = _g_string_new(depcnt02, ";LS(DASH,1,DEPCN)");
            }
        }
    } else {
        if (TRUE == safety_contour) {
            depcnt02 = _g_string_new(depcnt02, ";LS(SOLD,2,DEPSC)");
        } else {
            depcnt02 = _g_string_new(depcnt02, ";LS(SOLD,1,DEPCN)");
        }
    }

    if (TRUE == safety_contour) {
        S57_setScamin(geo, INFINITY);
        depcnt02 = g_string_prepend(depcnt02, ";OP(8OD13010)");
    } else {
        depcnt02 = g_string_prepend(depcnt02, ";OP(---33020)");
    }

    /* depth label (facultative in S-52)
    GString *sndfrm02 = _SNDFRM02(geo, depth_value);
    depcnt02 = g_string_append(depcnt02, sndfrm02->str);
    g_string_free(sndfrm02, TRUE);
    */

    //PRINTF("DEBUG: depth= %f\n", depth_value);

    return depcnt02;
}

static GString *DEPCNT03 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(DEPCNT03) --> CS(DEPCNT02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return DEPCNT02(geo);
}

static double   _DEPVAL01(S57_geo *geo, double least_depth)
// Remarks: S-57 Appendix B1 Annex A requires in Section 6 that areas of rocks be
// encoded as area obstruction, and that area OBSTRNs and area WRECKS
// be covered by either group 1 object DEPARE or group 1 object UNSARE.
// If the value of the attribute VALSOU for an area OBSTRN or WRECKS
// is missing, the DRVAL1 of an underlying DEPARE is the preferred default
// for establishing a depth vale. This procedure either finds the shallowest
// DRVAL1 of the one or more underlying DEPAREs, or returns an
// "unknown" depth value to the main procedure for the next default
// procedure.
{
    // Note: collect group 1 area DEPARE & DRGARE that touch this point/line/area is done at load-time
    GString *drval1str = NULL;
    S57_geo *geoTouch  = S57_getTouchDEPVAL(geo);
    if (NULL == geoTouch) {
        PRINTF("DEBUG: NULL geo _DEPVAL01/getTouchDEPVAL\n");
        //return UNKNOWN;
    } else {
        // Note: if an UNSARE is found then all other underlying objects can be ignore
        if (0 == g_strcmp0(S57_getName(geoTouch), "UNSARE")) {
            // debug
            PRINTF("DEBUG: %s:%c\n", S57_getName(geoTouch), S57_getObjtype(geoTouch));
            //g_assert(0);
            return UNKNOWN_DEPTH;
        }

        drval1str = S57_getAttVal(geoTouch, "DRVAL1");
    }
    double drval1 = (NULL==drval1str) ? UNKNOWN_DEPTH : S52_atof(drval1str->str);


    //least_depth = drval1; // !?! clang - val in least_depth never read

    if (UNKNOWN_DEPTH != drval1) {
        //if (least_depth < drval1) {  // SDUC BUG
        // FIXME: this branch never used because depth=drval1 above
        if (least_depth > drval1) {   // chenzunfeng fix
            least_depth = drval1;
            PRINTF("DEBUG: chenzunfeng found this bug: 'least_depth<drval1' (should be '>='), %s:%i\n", S57_getName(geo), S57_getS57ID(geo));
            //S57_highlightON(geo);
            g_assert(0);
        }

    }

    if (UNKNOWN_DEPTH != least_depth) {
        // adjuste datum
        least_depth += S52_MP_get(S52_MAR_DATUM_OFFSET);
    }

    return least_depth;
}

#if 0
//*
static double   _DEPVAL02(S57_geo *geo, double least_depth)
// PLib-4.0 draft
// Remarks: If the value of the attribute VALSOU for a wreck, rock or obstruction is
//          missing/unknown, CSP DEPVAL will establish a default 'LEAST_DEPTH'
//          from the attribute DRVAL1 of the underlying depth area, and
//          pass it to conditional procedures OBSTRN and WRECKS. However
//          this procedure is not valid if the value of EXPSOU for the object is 2
//          (object is shoaler than the DRVAL1 of the surrounding depth area), or
//          is unknown. It is also not valid if the value of WATLEV for the object is
//          other than 3 (object is always underwater). In either of these cases
//          the default procedures in conditional procedures OBSTRN and
//          WRECKS are used.
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: _DEPVAL02 --> _DEPVAL01\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return _DEPVAL01(geo, least_depth);
}
//*/
#endif  // 0

static GString *LEGLIN02 (S57_geo *geo)
// Remarks: The course of a leg is given by its start and end point. Therefore this
// conditional symbology procedure calculates the course and shows it
// alongside the leg. It also places the "distance to run" labels and cares for the
// different presentation of planned & alternate legs.
{
    GString *leglin02  = _g_string_new(NULL, NULL);
    GString *selectstr = S57_getAttVal(geo, "select");
    GString *plnspdstr = S57_getAttVal(geo, "plnspd");

    // FIXME: add OP()
    if ((NULL!=selectstr) && ('1'==*selectstr->str)) {
        g_string_append(leglin02, ";SY(PLNSPD03);LC(PLNRTE03)");
        // LUCM 42210 DISPLAYBASE
    } else {
        // alternate or undefined (check the later)
        g_string_append(leglin02, ";SY(PLNSPD04);LS(DOTT,2,APLRT)");
        // LUCM 52210 STANDARD
    }

    // TX: cog, course made good (mid point of leg)
    //g_string_append(leglin02, ";TX(leglin,3,1,2,'15112',0,0,CHBLK,51)"); // bsize = 12, too big
    g_string_append(leglin02, ";TX(leglin,3,1,2,'15111',0,0,CHBLK,51)");  // bsize = 11,

    // TX: plnspd, planned speed (mid point of leg)
    if ((NULL!=plnspdstr) && (0.0<S52_atof(plnspdstr->str))) {
        g_string_append(leglin02, ";TX(plnspd,1,2,2,'15110',0,0,CHBLK,51)");
    }

    /* FIXME: move to GL
    // TX: distance tags
    //if (0.0 < S52_MP_get(S52_MAR_DISTANCE_TAGS)) {
        g_string_append(leglin02, ";SY(PLNPOS02);TX(_disttags,3,1,2,'15112',0,0,CHBLK,51)");
    }
    */

    return leglin02;
}

static GString *LEGLIN03 (S57_geo *geo)
// Remarks: The course of a leg is given by its start and end point. Therefore this
// conditional symbology procedure calculates the course and shows it
// alongside the leg. It also places the "distance to run" labels and cares for the
// different presentation of planned & alternate legs.
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(LEGLIN03) switching to CS(LEGLIN02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }


    // next WP
    //SY(WAYPNT11)



    //PRINTF("Mariner's object not drawn\n");

    GString *leglin03 = LEGLIN02(geo);

    return leglin03;
}

static GString *_LITDSN01(S57_geo *geo);

static CCHAR   *_LIGHTS05_getSYcol(char *buf)
// WARNING: string must be store be the caller right after the call
{
    // FIXME: C1 3.1 use LIGHTS0x          and specs 3.2 use LIGHTS1x
    const char *sym = ";SY(LIGHTDEF";      //sym = ";SY(LITDEF11";

    // max 1 color
    if ('\0' == buf[1]) {
        if (_strpbrk(buf, "\003"))
            sym = ";SY(LIGHTS01";          //sym = ";SY(LIGHTS11";
        else if (_strpbrk(buf, "\004"))
            sym = ";SY(LIGHTS02";          //sym = ";SY(LIGHTS12";
        else if (_strpbrk(buf, "\001\006\013"))
            sym = ";SY(LIGHTS03";          //sym = ";SY(LIGHTS13";
    } else {
        // max 2 color
        if ('\0' == buf[2]) {
            if (_strpbrk(buf, "\001") && _strpbrk(buf, "\003"))
                sym = ";SY(LIGHTS01";      //sym = ";SY(LIGHTS11";
            else if (_strpbrk(buf, "\001") && _strpbrk(buf, "\004"))
                sym = ";SY(LIGHTS02";      //sym = ";SY(LIGHTS12";
        }
    }

    return sym;
}

static int      _LIGHTS05_cmpSector(S57_geo *geoA, S57_geo *geoB)
// return:
// +1 - A has a smaller sector,
//  0 - no overlap,
// -1 - B has a smaller sector
//
{
    // check for extend arc radius
    GString *Asectr1str = S57_getAttVal(geoA, "SECTR1");
    GString *Asectr2str = S57_getAttVal(geoA, "SECTR2");
    GString *Bsectr1str = S57_getAttVal(geoB, "SECTR1");
    GString *Bsectr2str = S57_getAttVal(geoB, "SECTR2");

    // check if sector present
    if (NULL==Asectr1str || NULL==Asectr2str ||  // redundant - caller did this allready
        NULL==Bsectr1str || NULL==Bsectr2str)
    {
        return 0;
    }

    // A/B sectors
    double Asectr1 = S52_atof(Asectr1str->str);
    double Asectr2 = S52_atof(Asectr2str->str);
    double Bsectr1 = S52_atof(Bsectr1str->str);
    double Bsectr2 = S52_atof(Bsectr2str->str);

    // FIXME: '<' or '<=
    // debug - force same sector test case
    //Bsectr1 = Asectr1;
    //Bsectr2 = Asectr2;

    // handle negative sweep
    double Asectr2tmp = Asectr2;
    double Bsectr2tmp = Bsectr2;

    if (Asectr1 > Asectr2) Asectr2tmp += 360.0;
    if (Bsectr1 > Bsectr2) Bsectr2tmp += 360.0;

    // A/B sweeps
    double Asweep = Asectr2tmp - Asectr1;
    double Bsweep = Bsectr2tmp - Bsectr1;

    if (Asweep >= 360.0) Asweep -= 360.0;
    if (Bsweep >= 360.0) Bsweep -= 360.0;

    // handle SECTR1/2 that overlap North
    double AsectorHead = Asectr1 + Asweep;
    double AsectorTail = Asectr2 - Asweep;
    double BsectorHead = Bsectr1 + Bsweep;
    double BsectorTail = Bsectr2 - Bsweep;

    // FIXME: '<' or '<= - need a good test case
    if ((AsectorTail<Bsectr1 && Bsectr1<Asectr2)     || (AsectorTail<Bsectr2 && Bsectr2<Asectr2)     ||
        (Asectr1    <Bsectr1 && Bsectr1<AsectorHead) || (Asectr1    <Bsectr2 && Bsectr2<AsectorHead) ||
        // same but reverse B/A
        (BsectorTail<Asectr1 && Asectr1<Bsectr2)     || (BsectorTail<Asectr2 && Asectr2<Bsectr2)     ||
        (Bsectr1    <Asectr1 && Asectr1<BsectorHead) || (Bsectr1    <Asectr2 && Asectr2<BsectorHead))

    /* '<=' break continous sectors
    if ((AsectorTail<=Bsectr1 && Bsectr1<=Asectr2)     || (AsectorTail<=Bsectr2 && Bsectr2<=Asectr2)     ||
        (Asectr1    <=Bsectr1 && Bsectr1<=AsectorHead) || (Asectr1    <=Bsectr2 && Bsectr2<=AsectorHead) ||
        // same but reverse B/A
        (BsectorTail<=Asectr1 && Asectr1<=Bsectr2)     || (BsectorTail<=Asectr2 && Asectr2<=Bsectr2)     ||
        (Bsectr1    <=Asectr1 && Asectr1<=BsectorHead) || (Bsectr1    <=Asectr2 && Asectr2<=BsectorHead))
    */

    {   // sector do overlap

        // B dominant - ()?:;
        if (Asweep < Bsweep) {
            return  1;
        } else {
            return -1;
        }
    }

    return 0;
}


static GString *LIGHTS05 (S57_geo *geo)
// Remarks: A light is one of the most complex S-57 objects. Its presentation depends on
// whether it is a light on a floating or fixed platform, its range, it's colour and
// so on. This conditional symbology procedure derives the correct
// presentation from these parameters and also generates an area that shows the
// coverage of the light.
//
// Notes on light sectors:
// 1.) The radial leg-lines defining the light sectors are normally drawn to only 25mm
// from the light to avoid clutter (see Part C). However, the mariner should be able to
// select "full light-sector lines" and have the leg-lines extended to the nominal range
// of the light (VALMAR).
//
// 2.) Part C of this procedure symbolizes the sectors at the light itself. In addition,
// it should be possible, upon request, for the mariner to be capable of identifying
// the colour and sector limit lines of the sectors affecting the ship even if the light
// itself is off the display.
// [ed. last sentence in bold]

// Note: why is this relationship not already encoded in S57 (ei. C_AGGR or C_STAC) ?

{
    GString *lights05          = _g_string_new(NULL, NULL);
    //GString *valnmrstr         = S57_getAttVal(geo, "VALNMR");
    //double   valnmr            = 0.0;
    GString *catlitstr         = S57_getAttVal(geo, "CATLIT");
    char     catlit[LISTSIZE]  = {'\0'};
    int      flare_at_45       = FALSE;
    //int      extend_arc_radius = TRUE;
    GString *sectr1str         = NULL;
    GString *sectr2str         = NULL;
    double   sectr1            = 0.0;
    double   sectr2            = 0.0;
    GString *colourstr         = NULL;
    char     colist[LISTSIZE]  = {'\0'};   // colour list
    GString *orientstr         = NULL;
    double   sweep             = 0.0;

    // Note: valmnr is only use when rendering
    //valnmr = (NULL == valnmrstr) ? 9.0 : S52_atof(valnmrstr->str);

    if ( NULL != catlitstr) {
        _parseList(catlitstr->str, catlit);

        // FIXME: OR vs AND/OR
        if (_strpbrk(catlit, "\010\013")) {
            g_string_append(lights05, ";SY(LIGHTS82)");
            return lights05;
        }

        if (_strpbrk(catlit, "\011")) {
            g_string_append(lights05, ";SY(LIGHTS81)");
            return lights05;
        }

        // bail out if this light is an emergecy light
        if (_strpbrk(catlit, "\021")) {
            return lights05;
        }

        if (_strpbrk(catlit, "\001\020")) {
            orientstr = S57_getAttVal(geo, "ORIENT");
            if (NULL != orientstr) {
                // FIXME: create a geo object (!?) LINE of lenght VALNMR
                // using ORIENT (from seaward) & POINT_T position
                g_string_append(lights05, ";LS(DASH,1,CHBLK)");
            }
        }
    }

    // Continuation A
    colourstr = S57_getAttVal(geo, "COLOUR");
    if (NULL != colourstr)
        _parseList(colourstr->str, colist);
    else {
        colist[0] = '\014';  // maganta (12)
        colist[1] = '\000';
    }

    sectr1str = S57_getAttVal(geo, "SECTR1");
    sectr1    = (NULL == sectr1str) ? 0.0 : S52_atof(sectr1str->str);
    sectr2str = S57_getAttVal(geo, "SECTR2");
    sectr2    = (NULL == sectr2str) ? 0.0 : S52_atof(sectr2str->str);

    if (NULL==sectr1str || NULL==sectr2str) {
        // not a sector light

        if (NULL != S57_getTouchLIGHTS(geo)) {
            if (_strpbrk(colist, "\001\005\013"))
                flare_at_45 = TRUE;
        }

        if (_strpbrk(catlit, "\001\020")) {
            if (NULL != orientstr){
                g_string_append(lights05, _LIGHTS05_getSYcol(colist));
                g_string_sprintfa(lights05, ",%s)", orientstr->str);
                g_string_append(lights05, ";TE('%03.0lf deg','ORIENT',3,3,3,'15110',3,1,CHBLK,23)" );
            } else
                g_string_append(lights05, ";SY(QUESMRK1)");
        } else {
            g_string_append(lights05, _LIGHTS05_getSYcol(colist));
            if (flare_at_45)
                g_string_append(lights05, ", 45)");
            else
                g_string_append(lights05, ",135)");
        }

        GString *litdsn01 = _LITDSN01(geo);
        if (NULL != litdsn01){
            g_string_append(lights05, ";TX('");
            g_string_append(lights05, litdsn01->str);
            _g_string_free(litdsn01, TRUE);

            if (flare_at_45)
                g_string_append(lights05, "',3,3,3,'15110',2,-1,CHBLK,23)" );
            else
                g_string_append(lights05, "',3,2,3,'15110',2,0,CHBLK,23)" );
        }

        return lights05;
    }

    // Continuation B - sector light
    if (NULL != sectr1str) {
        sweep = (sectr1 > sectr2) ? sectr2-sectr1+360 : sectr2-sectr1;
    }

    if (sweep<1.0 || sweep==360.0) {
        // handle all-around light

        g_string_append(lights05, _LIGHTS05_getSYcol(colist));
        g_string_append(lights05, ",135)");

        GString *litdsn01 = _LITDSN01(geo);
        if (NULL != litdsn01) {
            g_string_append(lights05, ";TX('");
            g_string_append(lights05, litdsn01->str);
            g_string_append(lights05, "',3,2,3,'15110',2,0,CHBLK,23)" );
            _g_string_free(litdsn01, TRUE);
        }

        return lights05;
    } else {
        // sector light: set sector legs
        // Note: 'LEGLEN' = 'VALNMR' or 'LEGLEN' = 25mm is done in _renderLS_LIGHTS05()
        g_string_append(lights05, ";LS(DASH,1,CHBLK)");
    }

    // check if LIGHT sector need processing
    GString *extendstr = S57_getAttVal(geo, "_extend_arc_radius");
    if ((NULL==extendstr) || ('N'==*extendstr->str)) {

        // init sector light overlap flag as a failsafe (NO extend radius)
        S57_setAtt(geo, "_extend_arc_radius", "N");

        for (S57_geo *geoTouch=S57_getTouchLIGHTS(geo); geoTouch!=NULL; geoTouch=S57_getTouchLIGHTS(geoTouch)) {
            int overlap = _LIGHTS05_cmpSector(geo, geoTouch);

            if (1 == overlap) {
                S57_setAtt(geo,      "_extend_arc_radius", "Y");
                S57_setAtt(geoTouch, "_extend_arc_radius", "N");
            }
            if (-1 == overlap) {
                S57_setAtt(geo,      "_extend_arc_radius", "N");
                S57_setAtt(geoTouch, "_extend_arc_radius", "Y");
            }
        }
    }

    // setup sector
    {
        char litvis[LISTSIZE] = {'\0'};  // light visibility
        GString *litvisstr = S57_getAttVal(geo, "LITVIS");

        // get light vis.
        if (NULL != litvisstr)
            _parseList(litvisstr->str, litvis);

        // faint light
        // FIXME: spec say OR (ie 1 number) the code is AND/OR
        if (_strpbrk(litvis, "\003\007\010")) {
            // Note: LS(DASH,1,CHBLK)
            // pass flag to _renderAC()

            // FIXME: what is that !? 'faint_light' is not used anywhere
            // find specs for this
            //g_string_append(lights05, ";AC(CHBLK)");
            //S57_setAtt(geo, "_faint_light", "Y");

            // sector leg - logic is _renderLS()
            g_string_append(lights05, ";LS(DASH,1,CHBLK)");

        } else {
            // set arc colour
            const char *sym = ";AC(CHMGD)";  // other

            // max 1 color
            if ('\0' == colist[1]) {
                if (_strpbrk(colist, "\003"))
                    sym = ";AC(LITRD)";
                else if (_strpbrk(colist, "\004"))
                    sym = ";AC(LITGN)";
                else if (_strpbrk(colist, "\001\006\013"))
                    sym = ";AC(LITYW)";
            } else {
                // max 2 color
                if ('\0' == colist[2]) {
                    if (_strpbrk(colist, "\001") && _strpbrk(colist, "\003"))
                        sym = ";AC(LITRD)";
                    else if (_strpbrk(colist, "\001") && _strpbrk(colist, "\004"))
                        sym = ";AC(LITGN)";
                }
            }

            g_string_append(lights05, sym);
        }
    }

    return lights05;
}

static GString *LIGHTS06 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(LIGHTS06) --> CS(LIGHTS05)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return LIGHTS05(geo);
}


static GString *_LITDSN01(S57_geo *geo)
// Remarks: In S-57 the light characteristics are held as a series of attributes values. The
// mariner may wish to see a light description text string displayed on the
// screen similar to the string commonly found on a paper chart. This
// conditional procedure, reads the attribute values from the above list of
// attributes and composes a light description string which can be displayed.
// This procedure is provided as a C function which has as input, the above
// listed attribute values and as output, the light description.
{
    GString *litdsn01         = _g_string_new(NULL, NULL);
    GString *gstr             = NULL;  // tmp
    GString *catlitstr        = S57_getAttVal(geo, "CATLIT");
    //char     catlit[LISTSIZE] = {'\0'};
    GString *litchrstr        = S57_getAttVal(geo, "LITCHR");
    //char     litchr[LISTSIZE] = {'\0'};
    GString *colourstr        = S57_getAttVal(geo, "COLOUR");
    //char     colour[LISTSIZE] = {'\0'};
    GString *statusstr        = S57_getAttVal(geo, "STATUS");
    //char     status[LISTSIZE] = {'\0'};

    // FIXME: need grammar to create light's text

    // CATLIT, LITCHR, COLOUR, HEIGHT, LITCHR, SIGGRP, SIGPER, STATUS, VALNMR

    // debug
    //if (3154 == S57_getS57ID(geo)) {
    //    PRINTF("lights found         XXXXXXXXXXXXXXXXXXXXXXX\n");
    //}


    // CATLIT
    if (NULL != catlitstr) {
        const char *tmp     = NULL;
        int         i       = 0;
        int         ncatlit = 0;
        char        catlit[LISTSIZE] = {'\0'};

        ncatlit = _parseList(catlitstr->str, catlit);

        //if (1 < _parseList(catlitstr->str, catlit))
        //    PRINTF("WARNING: more then one 'category of light' (CATLIT), other not displayed (%s)\n", catlitstr->str);

        while (i < ncatlit) {
            switch (catlit[i]) {
                // CATLIT attribute has no value!
                case 0: break;

                //1: directional function    IP 30.1-3;  475.7;
                case 1: tmp = "Dir "; break;

                //2: rear/upper light
                //3: front/lower light
                case 3:
                //4: leading light           IP 20.1-3;  475.6;
                case 4: break;

                //5: aero light              IP 60;      476.1;
                case 5: tmp = "Aero "; break;

                //6: air obstruction light   IP 61;      476.2;
                case 6: tmp = "Aero "; break;                    // CHS chart1.pdf (INT)
                //7: fog detector light      IP 62;      477;
                //8: flood light             IP 63;      478.2;
                //9: strip light             IP 64;      478.5;
                //10: subsidiary light        IP 42;      471.8;
                //11: spotlight
                //12: front
                case 12: break;
                //13: rear
                case 13: break;
                //14: lower
                //15: upper
                //16: moire effect         IP 31;      475.8;

                //17: emergency (bailout because this text overight the good one)
                case 17:
                    _g_string_free(litdsn01, TRUE);
                    //litdsn01 = NULL;
                    return NULL;
                    //break;

                //18: bearing light                       478.1;
                //19: horizontally disposed
                //20: vertically disposed

                default:
                    // FIXME: what is a good default
                    // or should it be left empty!
                    tmp = "FIXME:CATLIT ";
                    //PRINTF("DEBUG: no abreviation for CATLIT (%i)\n", catlit[0]);
            }
            ++i;
        }
        if (NULL != tmp)
            g_string_append(litdsn01, tmp);
    }


    // LITCHR
    if (NULL != litchrstr) {
        const char *tmp = NULL;
        char        litchr[LISTSIZE] = {'\0'};

        if (1 < _parseList(litchrstr->str, litchr)) {
            PRINTF("WARNING: more then one 'light characteristic' (LITCHR), other not displayed\n");
            g_assert(0);
        }

        switch (litchr[0]) {
            //1: fixed                             IP 10.1;
            case 1: tmp = "F"; break;
            //2: flashing                          IP 10.4;
            case 2: tmp = "Fl"; break;
            //3: long-flashing                     IP 10.5;
            case 3: tmp = "LFl"; break;
            //4: quick-flashing                    IP 10.6;
            case 4: tmp = "Q";   break;
            //5: very quick-flashing               IP 10.7;
            case 5: tmp = "VQ"; break;
            //6: ultra quick-flashing              IP 10.8;
            case 6: tmp = "UQ"; break;
            //7: isophased                         IP 10.3;
            case 7: tmp = "Iso"; break;
            //8: occulting                         IP 10.2;
            case 8: tmp = "Oc"; break;
            //9: interrupted quick-flashing        IP 10.6;
            case 9: tmp = "IQ"; break;
            //10: interrupted very quick-flashing   IP 10.7;
            case 10: tmp = "IVQ"; break;
            //11: interrupted ultra quick-flashing  IP 10.8;
            case 11: tmp = "IUQ"; break;
            //12: morse                             IP 10.9;
            case 12: tmp = "Mo"; break;
            //13: fixed/flash                       IP 10.10;
            case 13: tmp = "FFl"; break;
            //14: flash/long-flash
            case 14: tmp = "Fl+LFl"; break;
            // FIXME: not mention of 'alternating' occulting/flash in S57 attributes
            // but S52 say 'alternating occulting/flash' (p. 188)
            //15: occulting/flash
            case 15: tmp = "AlOc Fl"; break;
            //16: fixed/long-flash
            case 16: tmp = "FLFl"; break;
            //17: occulting alternating
            case 17: tmp = "AlOc"; break;
            //18: long-flash alternating
            case 18: tmp = "AlLFl"; break;
            //19: flash alternating
            case 19: tmp = "AlFl"; break;
            //20: group alternating
            case 20: tmp = "Al"; break;

            //21: 2 fixed (vertical)
            //22: 2 fixed (horizontal)
            //23: 3 fixed (vertical)
            //24: 3 fixed (horizontal)

            //25: quick-flash plus long-flash
            case 25: tmp = "Q+LFl"; break;
            //26: very quick-flash plus long-flash
            case 26: tmp = "VQ+LFl"; break;
            //27: ultra quick-flash plus long-flash
            case 27: tmp = "UQ+LFl"; break;
            //28: alternating
            case 28: tmp = "Al"; break;
            //29: fixed and alternating flashing
            case 29: tmp = "AlF Fl"; break;

            default:
                // FIXME: what is a good default
                // or should it be left empty!
                tmp = "FIXME:LITCHR ";
                //PRINTF("WARNING: no abreviation for LITCHR (%i)\n", litchr[0]);
        }
        g_string_append(litdsn01, tmp);
    }

    // SIGGRP, (c)(c) ..., SIGnal light GRouPing
    gstr = S57_getAttVal(geo, "SIGGRP");
    if (NULL != gstr) {
        //PRINTF("WARNING: SIGGRP not translated into text (%s)\n", gstr->str);
        g_string_append(litdsn01, gstr->str);
        //g_string_append(litdsn01, " ");
    }

    // COLOUR,
    if (NULL != colourstr) {
        const char *tmp = NULL;
        char        colour[LISTSIZE] = {'\0'};

        //if (1 < _parseList(colourstr->str, colour))
        //    PRINTF("WARNING: more then one 'colour' (COLOUR), other not displayed\n");
        int ncolor = _parseList(colourstr->str, colour);
        for (int i=0; i<ncolor; ++i) {
            switch (colour[0]) {
                //1: white   IP 11.1;    450.2-3;
                case 1: tmp = "W"; break;

                //2: black

                //3: red     IP 11.2;   450.2-3;
                case 3: tmp = "R"; break;
                //4: green   IP 11.3;   450.2-3;
                case 4: tmp = "G"; break;
                //5: blue    IP 11.4;   450.2-3;
                case 5: tmp = "Bu"; break;        // CHS chart1.pdf (INT)
                //6: yellow  IP 11.6;   450.2-3;
                case 6: tmp = "Y"; break;

                //7: grey
                //8: brown

                //9: amber   IP 11.8;   450.2-3;
                case 9:  tmp = "Am"; break;       // CHS chart1.pdf (INT)
                //10: violet  IP 11.5;   450.2-3;
                case 10: tmp = "Vi"; break;       // CHS chart1.pdf (INT)
                //11: orange  IP 11.7;   450.2-3;
                case 11: tmp = "Or"; break;       // CHS chart1.pdf (INT)
                //12: magenta
                //13: pink

                default:
                    // FIXME: what is a good default
                    // or should it be left empty!
                    tmp = "FIXME:COLOUR ";
                    //PRINTF("WARNING: no abreviation for COLOUR (%i)\n", colour[0]);
            }
            g_string_append(litdsn01, tmp);
        }
        g_string_append(litdsn01, " ");
    }

    // SIGPER, xx.xx, SIGnal light PERiod
    gstr = S57_getAttVal(geo, "SIGPER");
    if (NULL != gstr) {
        //PRINTF("WARNING: SIGPER not translated into text (%s)\n", gstr->str);
        g_string_append(litdsn01, gstr->str);
        g_string_append(litdsn01, "s ");
    }

    // HEIGHT, xxx.x
    gstr = S57_getAttVal(geo, "HEIGHT");
    if (NULL != gstr) {
        if (0.0 != S52_MP_get(S52_MAR_DATUM_OFFSET)) {
            // adjuste datum
            double height = S52_atof(gstr->str);
            height -= S52_MP_get(S52_MAR_DATUM_OFFSET);

            char str[8] = {0};
            g_snprintf(str, 8, "%.1fm ", height);
            g_string_append(litdsn01, str);
        } else {
            g_string_append(litdsn01, gstr->str);
            g_string_append(litdsn01, "m ");
        }
    }

    // VALNMR, xx.x
    gstr = S57_getAttVal(geo, "VALNMR");
    if (NULL != gstr) {
        // BUG: GDAL rounding problem (some value is 14.99 instead of 15)
        if (gstr->len > 3) {
            char str[5];
            // reformat in full
            g_snprintf(str, 3, "%3.1f", S52_atof(gstr->str));
            g_string_append(litdsn01, str);
        } else {
            //PRINTF("VALNMR:%s\n", gstr->str);
            g_string_append(litdsn01, gstr->str);
        }

        // FIXME: conflict in specs, nominal range in nautical miles in S57
        // S52 imply that it can be express in meter
        g_string_append(litdsn01, "M");
    }

    // STATUS,
    if (NULL != statusstr) {
        const char *tmp = NULL;
        char        status[LISTSIZE] = {'\0'};

        if (1 < _parseList(statusstr->str, status))
            PRINTF("WARNING: more then one 'status' (STATUS), other not displayed\n");

        switch (status[0]) {
            //1: permanent

            //2: occasional             IP 50;  473.2;
            case 2: tmp = "occas"; break;

            //3: recommended            IN 10;  431.1;
            //4: not in use             IL 14, 44;  444.7;
            //5: periodic/intermittent  IC 21; IQ 71;   353.3; 460.5;
            //6: reserved               IN 12.9;

            //7: temporary              IP 54;
            case 7: tmp = "temp"; break;
            //8: private                IQ 70;
            case 8: tmp = "priv"; break;

            //9: mandatory
            //10: destroyed/ruined

            //11: extinguished
            case 11: tmp = "exting"; break;

            //12: illuminated
            //13: historic
            //14: public
            //15: synchronized
            //16: watched
            //17: un-watched
            //18: existence doubtful
            default:
                // FIXME: what is a good default
                // or should it be left empty!
                tmp = "FIXME:STATUS ";
                //PRINTF("WARNING: no abreviation for STATUS (%i)\n", status[0]);
        }
        g_string_append(litdsn01, tmp);
    }

    return litdsn01;
}

// forward decl.
static GString *_UDWHAZ03(S57_geo *geo, double depth_value);
static GString *_QUAPNT01(S57_geo *geo);
static GString *OBSTRN04 (S57_geo *geo)
// Remarks: Obstructions or isolated underwater dangers of depths less than the safety
// contour which lie within the safe waters defined by the safety contour are
// to be presented by a specific isolated danger symbol and put in IMO
// category DISPLAYBASE (see (3), App.2, 1.3). This task is performed
// by the sub-procedure "UDWHAZ03" which is called by this symbology
// procedure. Objects of the class "under water rock" are handled by this
// routine as well to ensure a consistent symbolization of isolated dangers on
// the seabed.
//
// Note: updated to Cs1_md.pdf (ie was OBSTRN03)

{
    GString *obstrn04 = _g_string_new(NULL, NULL);
    GString *sndfrm02 = NULL;
    GString *udwhaz03 = NULL;

    GString *valsoustr   = S57_getAttVal(geo, "VALSOU");
    double   valsou      = UNKNOWN_DEPTH;
    double   depth_value = UNKNOWN_DEPTH;  // clang - init val never read
    double   least_depth = UNKNOWN_DEPTH;

    if (NULL != valsoustr) {
        valsou      = S52_atof(valsoustr->str);
        depth_value = valsou;
        sndfrm02    = _SNDFRM02(geo, depth_value);
    } else {
        if (S57_AREAS_T == S57_getObjtype(geo))
            least_depth = _DEPVAL01(geo, UNKNOWN_DEPTH);

        //if (UNKNOWN != least_depth) {  // <<< SDUC DEBUG to skip symbol ISODGR01 in shallow water
        if (UNKNOWN_DEPTH == least_depth) {    // chenzunfeng fix
#ifdef S52_DEBUG
            /*
            {
                static int silent = FALSE;
                if (FALSE == silent) {
                    PRINTF("DEBUG: chenzunfeng found this should be (UNKNOWN == least_depth)[not !=], %s:%i\n", S57_getName(geo), S57_getS57ID(geo));

                    silent = TRUE;
                    PRINTF("DEBUG: (this msg will not repeat)\n");
                }
                S57_setHighlight(geo, TRUE);  // Bic island CA279037.000
            }
            //*/
#endif

            GString *catobsstr = S57_getAttVal(geo, "CATOBS");
            GString *watlevstr = S57_getAttVal(geo, "WATLEV");

            if (NULL!=catobsstr && '6'==*catobsstr->str)
                depth_value = 0.01;
            else {
                if (NULL == watlevstr) {  // default
                    depth_value = -15.0;
                } else {
                    switch (*watlevstr->str){
                        case '5': depth_value =   0.0 ; break;
                        case '3': depth_value =   0.01; break;
                        case '4':
                        case '1':
                        case '2':
                        default : depth_value = -15.0 ; break;
                    }
                }
            }
        } else {
            depth_value = least_depth;
        }
    }

    udwhaz03 = _UDWHAZ03(geo, depth_value);

    if (S57_POINT_T == S57_getObjtype(geo)) {
        // Continuation A
        int      sounding = FALSE;
        GString *quapnt01 = _QUAPNT01(geo);

        if (NULL != udwhaz03){
            g_string_append(obstrn04, udwhaz03->str);
            if (NULL != quapnt01) {
                g_string_append(obstrn04, quapnt01->str);
                _g_string_free(quapnt01, TRUE);
            }

            _g_string_free(udwhaz03, TRUE);
            _g_string_free(sndfrm02, TRUE);

            return obstrn04;
        }

        if (UNKNOWN_DEPTH != valsou) {
            if (valsou <= 20.0) {
                GString *watlevstr = S57_getAttVal(geo, "WATLEV");
                if (0 == g_strcmp0(S57_getName(geo), "UWTROC")) {
                    if (NULL == watlevstr) {  // default
                        g_string_append(obstrn04, ";SY(DANGER01)");
                        sounding = TRUE;
                    } else {
                        switch (*watlevstr->str){
                            case '3': g_string_append(obstrn04, ";SY(DANGER01)"); sounding = TRUE ; break;
                            case '4':
                            case '5': g_string_append(obstrn04, ";SY(UWTROC04)"); sounding = FALSE; break;
                            default : g_string_append(obstrn04, ";SY(DANGER01)"); sounding = TRUE ; break;
                        }
                    }
                } else { // OBSTRN
                    if (NULL == watlevstr) { // default
                        g_string_append(obstrn04, ";SY(DANGER01)");
                        sounding = TRUE;
                    } else {
                        switch (*watlevstr->str) {
                            case '1':
                            case '2': g_string_append(obstrn04, ";SY(OBSTRN11)"); sounding = FALSE; break;
                            case '3': g_string_append(obstrn04, ";SY(DANGER01)"); sounding = TRUE;  break;
                            case '4':
                            case '5': g_string_append(obstrn04, ";SY(DANGER03)"); sounding = TRUE; break;
                            default : g_string_append(obstrn04, ";SY(DANGER01)"); sounding = TRUE; break;
                        }
                    }
                }
            } else {  // valsou > 20.0
                g_string_append(obstrn04, ";SY(DANGER02)");
                sounding = FALSE;
            }

        } else {  // NO valsou
                GString *watlevstr = S57_getAttVal(geo, "WATLEV");
                if (0 == g_strcmp0(S57_getName(geo), "UWTROC")) {
                    if (NULL == watlevstr)  // default
                       g_string_append(obstrn04, ";SY(UWTROC04)");
                    else {
                        if ('3' == *watlevstr->str)
                            g_string_append(obstrn04, ";SY(UWTROC03)");
                        else
                            g_string_append(obstrn04, ";SY(UWTROC04)");
                    }

                } else { // OBSTRN
                    if (NULL == watlevstr) // default
                        g_string_append(obstrn04, ";SY(OBSTRN01)");
                    else {
                        switch (*watlevstr->str) {
                            case '1':
                            case '2': g_string_append(obstrn04, ";SY(OBSTRN11)"); break;
                            case '3': g_string_append(obstrn04, ";SY(OBSTRN01)"); break;
                            case '4':
                            //case '5': g_string_append(obstrn04, ";SY(OBSTRN01)");
                            case '5': g_string_append(obstrn04, ";SY(OBSTRN03)");
#ifdef S52_DEBUG
                            {   // debug - check impact of this bug
                                // this change the color from blue to green
                                PRINTF("DEBUG: chenzunfeng found this should be SY(OBSTRN03)[not 01], %s:%i\n", S57_getName(geo), S57_getS57ID(geo));
                                S57_setHighlight(geo, TRUE);
                                //g_assert(0);  // CA479020.000 pass here
                            }
#endif
                                break;
                            default : g_string_append(obstrn04, ";SY(OBSTRN01)"); break;
                        }
                    }
                }

        }

        if (TRUE==sounding && NULL!=sndfrm02)
            g_string_append(obstrn04, sndfrm02->str);

        if (NULL != quapnt01)
            g_string_append(obstrn04, quapnt01->str);

        _g_string_free(udwhaz03, TRUE);
        _g_string_free(sndfrm02, TRUE);
        _g_string_free(quapnt01, TRUE);

        return obstrn04;

    } else {
        if (S57_LINES_T == S57_getObjtype(geo)) {
            // Continuation B
            GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
            //int      quapos    = 0;

            // ------------------------------------------------------------
            // FIXME: chenzunfeng found this bug: should be mutually exclusive
            // FIX: add 2 'else'
            if (NULL != quaposstr) {
                int quapos = S52_atoi(quaposstr->str);
                if (2 <= quapos && quapos < 10) {
                    if (NULL != udwhaz03)
                        g_string_append(obstrn04, ";LC(LOWACC41)");
                    else
                        g_string_append(obstrn04, ";LC(LOWACC31)");
                }
#ifdef S52_DEBUG
                {   // debug - check impact of this bug
                    PRINTF("DEBUG: chenzunfeng found this bug: should skip 'udwhaz03' & 'valsou', %s:%i\n", S57_getName(geo), S57_getS57ID(geo));
                    S57_setHighlight(geo, TRUE);
                }
#endif
            } else {
                if (NULL != udwhaz03) {
                    g_string_append(obstrn04, ";LS(DOTT,2,CHBLK)");

#ifdef S52_DEBUG
                    {   // debug - check impact of this bug
                        PRINTF("DEBUG: chenzunfeng found this bug: should skip 'valsou', %s:%i\n", S57_getName(geo), S57_getS57ID(geo));
                        S57_setHighlight(geo, TRUE);
                    }
#endif
               } else {
                    if (UNKNOWN_DEPTH != valsou) {
                        if (valsou <= 20.0)
                            g_string_append(obstrn04, ";LS(DOTT,2,CHBLK)");
                        else
                            g_string_append(obstrn04, ";LS(DASH,2,CHBLK)");
                    } else
                        g_string_append(obstrn04, ";LS(DOTT,2,CHBLK)");
                }
            }
            // ------------------------------------------------------------


            if (NULL != udwhaz03)
                g_string_append(obstrn04, udwhaz03->str);
            else {
                if (UNKNOWN_DEPTH != valsou) {
                    if (valsou<=20.0 && NULL!=sndfrm02)
                        g_string_append(obstrn04, sndfrm02->str);
                }
            }

            _g_string_free(udwhaz03, TRUE);
            _g_string_free(sndfrm02, TRUE);

            return obstrn04;

        } else {
            // Continuation C (AREAS_T)
            GString *quapnt01 = _QUAPNT01(geo);

            // FIX: chenzunfeng note that if NULL!=udwhaz03 doesn't meen to show FOULAR01
            if ((NULL!=udwhaz03) && (NULL!=strstr(udwhaz03->str, "ISODGR"))) {
                g_string_append(obstrn04, ";AC(DEPVS);AP(FOULAR01)");
                g_string_append(obstrn04, ";LS(DOTT,2,CHBLK)");
                g_string_append(obstrn04, udwhaz03->str);
                if (NULL != quapnt01) {
                    g_string_append(obstrn04, quapnt01->str);
                    _g_string_free(quapnt01, TRUE);
                }

                _g_string_free(udwhaz03, TRUE);
                _g_string_free(sndfrm02, TRUE);

                return obstrn04;
            }

            if (UNKNOWN_DEPTH != valsou) {
                // S52 BUG (see CA49995B.000:305859) we get here because
                // there is no color beside NODATA (ie there is a hole in group 1 area!)
                // and this mean there is still not AC() command at this point.
                // FIX 1: add danger.
                // g_string_append(obstrn04str, ";AC(DNGHL)");
                // FIX 2: do nothing and CA49995B.000:305859 will have NODATA
                // wich is normal for a test data set !


                if (valsou <= 20.0)
                    g_string_append(obstrn04, ";LS(DOTT,2,CHBLK)");
                else {
                    //g_string_append(obstrn04, ";LS(DASH,2,CHBLK)");  // SD BUG
                    g_string_append(obstrn04, ";LS(DASH,2,CHGRD)");

#ifdef S52_DEBUG
                    {   // debug - check impact of this bug
                        PRINTF("DEBUG: chenzunfeng found this bug LS(DASH,2,CHGRD)[not CHBLK], %s:%i\n", S57_getName(geo), S57_getS57ID(geo));
                        S57_setHighlight(geo, TRUE);
                        g_assert(0);  // FIXME: name ENC that pass here
                    }
#endif
                }

                if (NULL != sndfrm02)
                    g_string_append(obstrn04, sndfrm02->str);

            } else {
                // NO valsou
                GString *watlevstr = S57_getAttVal(geo, "WATLEV");
                if (NULL != watlevstr) {
                    GString *catobsstr = S57_getAttVal(geo, "CATOBS");
                    if ('3'==*watlevstr->str && NULL!=catobsstr && '6'==*catobsstr->str) {

                        PRINTF("DEBUG: S64 GB4X0000.000/GB5X01NE.000 pass here (%s:%i)\n", S57_getName(geo), S57_getS57ID(geo));
                        //g_assert(0);

                        // Note: LUP for OBSTRN:CATOBS6 --> CS(OBSTRN04);AP(FOULAR01);LS(DOTT,2,CHBLK)
                        //g_string_append(obstrn04, ";AC(DEPVS);AP(FOULAR01);LS(DOTT,2,CHBLK)");
                        g_string_append(obstrn04, ";AC(DEPVS)");

                        // debug - force display
                        //S57_setScamin(geo, INFINITY);

                    } else {
                        switch (*watlevstr->str) {
                            case '1':
                            case '2': g_string_append(obstrn04, ";AC(CHBRN);LS(SOLD,2,CSTLN)"); break;
                            case '4': g_string_append(obstrn04, ";AC(DEPIT);LS(DASH,2,CSTLN)"); break;
                            case '5':
                            case '3':
                            default : g_string_append(obstrn04, ";AC(DEPVS);LS(DOTT,2,CHBLK)");  break;
                        }
                    }
                } else {
                    // default
                    g_string_append(obstrn04, ";AC(DEPVS);LS(DOTT,2,CHBLK)");
                }
            }

            if (NULL != quapnt01)
                g_string_append(obstrn04, quapnt01->str);

            _g_string_free(udwhaz03, TRUE);
            _g_string_free(sndfrm02, TRUE);
            _g_string_free(quapnt01, TRUE);

            return obstrn04;
        }
    }

    // FIXME: check if one exit point could do? or goto exit !
    return NULL;
}

static GString *OBSTRN05 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(OBSTRN05) --> CS(OBSTRN04)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return OBSTRN04(geo);
}

static GString *OBSTRN06 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(OBSTRN06) --> CS(OBSTRN04)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return OBSTRN04(geo);
}

static GString *OWNSHP02 (S57_geo *geo)
// Remarks:
// 1. CONNING POSITION
//    1.1 When own-ship is drawn to scale, the conning position must be correctly located in
//        relation to the ship's outline. The conning position then serves as the pivot point for
//        the own-ship symbol, to be located by the ECDIS at the correct latitude, longitude
//        for the conning point, as computed from the positioning system, correcting for
//        antenna offset.
//    1.2 In this procedure it is assumed that the heading line, beam bearing line and course
//        and speed vector originate at the conning point. If another point of origin is used,
//        for example to account for the varying position of the ship�s turning centre, this must
//        be made clear to the mariner.
//
// 2. DISPLAY OPTIONS
//    2.1 Only the ship symbol is mandatory for an ECDIS. The mariner should be prompted
//        to select from the following additional optional features:
//    - display own-ship as:
//        1. symbol, or
//        2. scaled outline
//    - select time period determining vector length for own-ship and other vessel course and speed
//      vectors, (all vectors must be for the same time period),
//    - display own-ship vector,
//    - select ground or water stabilization for all vectors, and select whether to display the type of
//      stabilization, (by arrowhead),
//    - select one-minute or six-minute vector time marks,
//    - select whether to show a heading line, to the edge of the display window,
//    - select whether to show a beam bearing line, and if so what length (default: 10mm total
//      length)

// Note: attribure used:
//  shpbrd: Ship's breadth (beam),
//  shplen: Ship's length over all,
//  headng: Heading,
//  cogcrs: Course over ground,
//  sogspd: Speed over ground,
//  ctwcrs: Course through water,
//  stwspd: Speed through water,

// FIXME: get conning position (where/how to get those values?)
{
    GString *ownshp02  = _g_string_new(NULL, NULL);
    //GString *headngstr = S57_getAttVal(geo, "headng");
    GString *vlabelstr = S57_getAttVal(geo, "_vessel_label");

    // FIXME: move to GL
    // experimental: text label
    if (NULL != vlabelstr) {
        g_string_append(ownshp02, ";TX(_vessel_label,3,3,3,'15110',1,1,SHIPS,75)" );
        g_string_append(ownshp02, ";TE('%03.0lf deg','cogcrs',3,3,3,'15109',1,2,SHIPS,77)" );
        g_string_append(ownshp02, ";TE('%3.1lf kts','sogspd',3,3,3,'15109',5,2,SHIPS,78)" );
    }

    // FIXME: 2 type of line for 3 line symbol - overdraw of 1px pen_w type
    // pen_w:
    //   2px - vector
    //   1px - heading, beam brg

    // draw to the edge of window
    // first LS() 1px
    //if (NULL != headngstr)
        g_string_append(ownshp02, ";LS(SOLD,1,SHIPS)");

    // draw OWNSHP05 if length > 10 mm, else OWNSHP01 (circle)
    //if (TRUE == S52_MP_get(S52_MAR_SHIPS_OUTLINE))
        g_string_append(ownshp02, ";SY(OWNSHP05)");

    g_string_append(ownshp02, ";SY(OWNSHP01)");


    // course / speed vector on ground / water
    //if (0.0 != S52_MP_get(S52_MAR_VECPER))  {
        // draw line according to ships course (cogcrs or ctwcrs) and speed (sogspd or stwspd)
        // Note: second LS() 2px
        //g_string_append(ownshp02, ";LS(SOLD,2,SHIPS)");
        g_string_append(ownshp02, ";SY(VECGND01);SY(VECWTR01);LS(SOLD,2,SHIPS)");

        /*
        // vector stabilisation (symb place at the end of vector)
        //if (0.0 != S52_MP_get(S52_MAR_VECSTB)) {
            // ground
            //if (1.0 == S52_MP_get(S52_MAR_VECSTB))
                g_string_append(ownshp02, ";SY(VECGND01)");

            // water
            //if (2.0 == S52_MP_get(S52_MAR_VECSTB))
                g_string_append(ownshp02, ";SY(VECWTR01)");
        }
        */

        // FIXME: move to GL
        // time mark (on vector)
        //if (0.0 != S52_MP_get(S52_MAR_VECMRK)) {
            // 6 min. and 1 min. symb.
            //if (1.0 == S52_MP_get(S52_MAR_VECMRK))
                g_string_append(ownshp02, ";SY(OSPSIX02);SY(OSPONE02)");

            // 6 min. symb
            //if (2.0 == S52_MP_get(S52_MAR_VECMRK))
            //    g_string_append(ownshp02, ";SY(OSPSIX02)");
        //}
    //}

    // beam bearing
    // Note: third LS() 1px
    //if (0.0 != S52_MP_get(S52_MAR_BEAM_BRG_NM)) {
        g_string_append(ownshp02, ";LS(SOLD,1,SHIPS)");
    //}

    return ownshp02;
}

static GString *PASTRK01 (S57_geo *geo)
// Remarks: This conditional symbology procedure was designed to allow the mariner
// to select time labels at the pasttrack (see (3) 10.5.11.1). The procedure also
// cares for the presentation of primary and secondary pasttrack.
//
// The manufacturer should define his own data class (spatial primitive) in xyt
// (position and time) in order to represent Pastrk.
{
    //PRINTF("Mariner's object not drawn\n");

    GString *pastrk01  = NULL;
    GString *catpststr = S57_getAttVal(geo, "catpst");

    if (NULL != catpststr) {
        // FIXME: view group: 1 - standard (52430) , 2 - standard (52460)
        // Note: text grouping 51
        if ('1' == *catpststr->str) {
            //pastrk01 = g_string_new(";LS(SOLD,2,PSTRK);SY(PASTRK01);TX(pastrk,2,1,2,'15110',-1,-1,CHBLK,51)");
            pastrk01 = _g_string_new(pastrk01, ";LS(SOLD,2,PSTRK);SY(PASTRK01)");
        }
        if ('2' == *catpststr->str) {
            //pastrk01 = g_string_new(";LS(SOLD,1,SYTRK);SY(PASTRK02);TX(pastrk,2,1,2,'15110',-1,-1,CHBLK,51)");
            pastrk01 = _g_string_new(pastrk01, ";LS(SOLD,1,SYTRK);SY(PASTRK02)");
        }

        // FIXME: if time-tags
        //if (0.0 < S52_MAR_TIME_TAGS) {
        //    g_string_append(pastrk01, ";TX(_timetags,2,1,2,'15110',-1,-1,CHBLK,51)");
        //}

    }

    return pastrk01;
}

static GString *_QUALIN01(S57_geo *geo);
static GString *QUAPOS01 (S57_geo *geo)
// Remarks: The attribute QUAPOS, which identifies low positional accuracy, is attached
// to the spatial object, not the feature object.
//
// This procedure passes the object to procedure QUALIN01 or QUAPNT01,
// which traces back to the spatial object, retrieves any QUAPOS attributes,
// and returns the appropriate symbolization to QUAPOS01.
{
    GString *quapos01 = NULL;

    if (S57_LINES_T == S57_getObjtype(geo))
        quapos01 = _QUALIN01(geo);
    else
        quapos01 = _QUAPNT01(geo);

    return quapos01;
}

static GString *_QUALIN01(S57_geo *geo)
// Remarks: The attribute QUAPOS, which identifies low positional accuracy, is attached
// only to the spatial component(s) of an object.
//
// A line object may be composed of more than one spatial object.
//
// This procedure looks at each of the spatial
// objects, and symbolizes the line according to the positional accuracy.
{
    GString *qualin01  = NULL;
    GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
    //int      quapos    = 0;
    const char *line   = NULL;

    if (NULL != quaposstr) {
        int quapos = S52_atoi(quaposstr->str);
        if ( 2 <= quapos && quapos < 10)
            line = ";LC(LOWACC21)";
    } else {
        /*
        GString *objlstr = S57_getAttVal(geo, "OBJL");
        int      objl    = (NULL == objlstr)? 0 : S52_atoi(objlstr->str);

        // debug --this should not trigger an assert since
        // there is no object number zero
        if (0 == objl) {
            PRINTF("ERROR: no OBJL\n");
            g_assert(0);
            return qualin01;
        }

        if (COALNE == objl) {
        */

        if (0 == g_strcmp0(S57_getName(geo), "COALNE")) {
            GString *conradstr = S57_getAttVal(geo, "CONRAD");

            if (NULL != conradstr) {
                if ('1' == *conradstr->str)
                    line = ";LS(SOLD,3,CHMGF);LS(SOLD,1,CSTLN)";
                else
                    line = ";LS(SOLD,1,CSTLN)";
            } else
                line = ";LS(SOLD,1,CSTLN)";

        } else  // LNDARE
            line = ";LS(SOLD,1,CSTLN)";
    }

    if (NULL != line) {
        qualin01 = _g_string_new(qualin01, line);
    }

    return qualin01;
}

static GString *_QUAPNT01(S57_geo *geo)
// Remarks: The attribute QUAPOS, which identifies low positional accuracy, is attached
// only to the spatial component(s) of an object.
//
// This procedure retrieves any QUAPOS attributes, and returns the
// appropriate symbols to the calling procedure.
{
    GString *quapnt01  = NULL;
    int      accurate  = TRUE;
    GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
    int      quapos    = (NULL == quaposstr)? 0 : S52_atoi(quaposstr->str);

    if (NULL != quaposstr) {
        if ( 2 <= quapos && quapos < 10)
            accurate = FALSE;
    }

    if (accurate) {
        quapnt01 = _g_string_new(quapnt01, ";SY(LOWACC01)");
    }

    return quapnt01;
}

static GString *SLCONS03 (S57_geo *geo)
// Remarks: Shoreline construction objects which have a QUAPOS attribute on their
// spatial component indicating that their position is unreliable are symbolized
// by a special linestyle in the place of the varied linestyles normally used.
// Otherwise this procedure applies the normal symbolization.
{
    GString *slcons03  = NULL;
    //GString *slcons03  = g_string_new("");
    GString *valstr    = NULL;
    CCHAR   *cmdw      = NULL;   // command word
    GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
    int      quapos    = (NULL == quaposstr)? 0 : S52_atoi(quaposstr->str);

    if (S57_POINT_T == S57_getObjtype(geo)) {
        if (NULL != quaposstr) {
            if (2 <= quapos && quapos < 10)
                cmdw =";SY(LOWACC01)";
        }
    } else {
        // LINE_T and AREA_T are the same
        if (NULL != quaposstr) {
            if (2 <= quapos && quapos < 10)
                cmdw =";LC(LOWACC01)";
        } else {
            valstr = S57_getAttVal(geo, "CONDTN");

            if (NULL != valstr && ( '1' == *valstr->str || '2' == *valstr->str))
                    cmdw = ";LS(DASH,1,CSTLN)";
            else {
                int val = 0;
                valstr  = S57_getAttVal(geo, "CATSLC");
                val     = (NULL == valstr)? 0 : S52_atoi(valstr->str);

                if (NULL != valstr && ( 6  == val || 15 == val || 16 == val ))
                        cmdw = ";LS(SOLD,4,CSTLN)";
                else {
                    valstr = S57_getAttVal(geo, "WATLEV");

                    if (NULL != valstr && '2' == *valstr->str)
                            cmdw = ";LS(SOLD,2,CSTLN)";
                    else
                        if (NULL != valstr && ('3' == *valstr->str || '4' == *valstr->str))
                            cmdw = ";LS(DASH,2,CSTLN)";
                        else
                            cmdw = ";LS(SOLD,2,CSTLN)";  // default

                }
            }
        }
    }

    // WARNING: not explicitly specified in S-52 !!
    // FIXME: this is to put AC(DEPIT) --intertidal area

    if (S57_AREAS_T == S57_getObjtype(geo)) {
        GString    *seabed01  = NULL;
        GString    *drval1str = S57_getAttVal(geo, "DRVAL1");
        double      drval1    = (NULL == drval1str)? UNKNOWN_DEPTH : S52_atof(drval1str->str);
        GString    *drval2str = S57_getAttVal(geo, "DRVAL2");
        double      drval2    = (NULL == drval2str)? UNKNOWN_DEPTH : S52_atof(drval2str->str);

        // adjuste datum
        if (UNKNOWN_DEPTH != drval1)
            drval1 += S52_MP_get(S52_MAR_DATUM_OFFSET);
        if (UNKNOWN_DEPTH != drval2)
            drval2 += S52_MP_get(S52_MAR_DATUM_OFFSET);

        // debug
        //PRINTF("***********drval1=%f drval2=%f \n", drval1, drval2);

        seabed01 = _SEABED01(drval1, drval2);
        if (NULL != seabed01) {
            slcons03 = _g_string_new(slcons03, seabed01->str);
            //g_string_append(slcons03, seabed01->str);
            _g_string_free(seabed01, TRUE);
        }

    }

    if (NULL != cmdw) {
        if (NULL == slcons03) {
            slcons03 = _g_string_new(slcons03, cmdw);
        } else {
            g_string_append(slcons03, cmdw);
        }
    }

    return slcons03;
}

static GString *RESARE02 (S57_geo *geo)
// Remarks: A list-type attribute is used because an area of the object class RESARE may
// have more than one category (CATREA). For example an inshore traffic
// zone might also have fishing and anchoring prohibition and a prohibited
// area might also be a bird sanctuary or a mine field.
//
// This conditional procedure is set up to ensure that the categories of most
// importance to safe navigation are prominently symbolized, and to pass on
// all given information with minimum clutter. Only the most significant
// restriction is symbolized, and an indication of further limitations is given by
// a subscript "!" or "I". Further details are given under conditional
// symbology procedure RESTRN01
//
// Other object classes affected by attribute RESTRN are handled by
// conditional symbology procedure RESTRN01.
{
    GString *resare02         = _g_string_new(NULL, NULL);
    GString *restrnstr        = S57_getAttVal(geo, "RESTRN");
    //char     restrn[LISTSIZE] = {'\0'};
    GString *catreastr        = S57_getAttVal(geo, "CATREA");
    char     catrea[LISTSIZE] = {'\0'};
    const char *symb          = NULL;
    const char *line          = NULL;
    const char *prio          = NULL;

    if ( NULL != restrnstr) {
        char restrn[LISTSIZE] = {'\0'};
        _parseList(restrnstr->str, restrn);

        if (NULL != catreastr) _parseList(catreastr->str, catrea);

        if (_strpbrk(restrn, "\007\010\016")) {
            // Continuation A
            if (_strpbrk(restrn, "\001\002\003\004\005\006"))
                symb = ";SY(ENTRES61)";
            else {
                if (NULL != catreastr && _strpbrk(catrea, "\001\010\011\014\016\023\025\031"))
                        symb = ";SY(ENTRES61)";
                else {
                    if (_strpbrk(restrn, "\011\012\013\014\015"))
                        symb = ";SY(ENTRES71)";
                    else {
                        if (NULL != catreastr && _strpbrk(catrea, "\004\005\006\007\012\022\024\026\027\030"))
                            symb = ";SY(ENTRES71)";
                        else
                            symb = ";SY(ENTRES51)";
                    }
                }
            }

            if (TRUE == (int) S52_MP_get(S52_MAR_SYMBOLIZED_BND))
                line = ";LC(CTYARE51)";
            else
                line = ";LS(DASH,2,CHMGD)";

            prio = ";OP(6---)";  // display prio set to 6


        } else {
            if (_strpbrk(restrn, "\001\002")) {
                // Continuation B
                if (_strpbrk(restrn, "\003\004\005\006"))
                    symb = ";SY(ACHRES61)";
                else {
                    if (NULL != catreastr && _strpbrk(catrea, "\001\010\011\014\016\023\025\031"))
                            symb = ";SY(ACHRES61)";
                    else {
                        if (_strpbrk(restrn, "\011\012\013\014\015"))
                            symb = ";SY(ACHRES71)";
                        else {
                            if (NULL != catreastr && _strpbrk(catrea, "\004\005\006\007\012\022\024\026\027\030"))
                                symb = ";SY(ACHRES71)";
                            else
                                symb = ";SY(ACHRES51)";
                        }
                    }
                }

                if (TRUE == (int) S52_MP_get(S52_MAR_SYMBOLIZED_BND))
                    line = ";LC(ACHRES51)";
                else
                    line = ";LS(DASH,2,CHMGD)";

                prio = ";OP(6---)";  // display prio set to 6

            } else {
                if (_strpbrk(restrn, "\003\004\005\006")) {
                    // Continuation C
                    if (NULL != catreastr && _strpbrk(catrea, "\001\010\011\014\016\023\025\031"))
                            symb = ";SY(FSHRES51)";
                    else {
                        if (_strpbrk(restrn, "\011\012\013\014\015"))
                            symb = ";SY(FSHRES71)";
                        else {
                            if (NULL != catreastr && _strpbrk(catrea, "\004\005\006\007\012\022\024\026\027\030"))
                                symb = ";SY(FSHRES71)";
                            else
                                symb = ";SY(FSHRES51)";
                        }
                    }

                    if (TRUE == (int) S52_MP_get(S52_MAR_SYMBOLIZED_BND))
                        line = ";LC(FSHRES51)";
                    else
                        line = ";LS(DASH,2,CHMGD)";

                    prio = ";OP(6---)";  // display prio set to 6

                } else {
                    if (_strpbrk(restrn, "\011\012\013\014\015"))
                        symb = ";SY(INFARE51)";
                    else
                        symb = ";SY(RSRDEF51)";

                    if (TRUE == (int) S52_MP_get(S52_MAR_SYMBOLIZED_BND))
                        line = ";LC(CTYARE51)";
                    else
                        line = ";LS(DASH,2,CHMGD)";

                }
            }
        }

    } else {
        // Continuation D
        if (NULL != catreastr) {
            if (_strpbrk(catrea, "\001\010\011\014\016\023\025\031")) {
                if (_strpbrk(catrea, "\004\005\006\007\012\022\024\026\027\030"))
                    symb = ";SY(CTYARE71)";
                else
                    symb = ";SY(CTYARE51)";
            } else {
                if (_strpbrk(catrea, "\004\005\006\007\012\022\024\026\027\030"))
                    symb = ";SY(INFARE71)";
                else
                    symb = ";SY(RSRDEF51)";
            }
        } else
            symb = ";SY(RSRDEF51)";

        if (TRUE == (int) S52_MP_get(S52_MAR_SYMBOLIZED_BND))
            line = ";LC(CTYARE51)";
        else
            line = ";LS(DASH,2,CHMGD)";
    }

    // create command word
    if (NULL != prio)
        g_string_append(resare02, prio);
    g_string_append(resare02, line);
    g_string_append(resare02, symb);

    return resare02;
}

static GString *RESARE03 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(RESARE03) --> CS(RESARE02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return RESARE02(geo);
}

static GString *RESTRN01 (S57_geo *geo)
// Remarks: Objects subject to RESTRN01 are actually symbolised in sub-process
// RESCSP01, since the latter can also be accessed from other conditional
// symbology procedures. RESTRN01 merely acts as a "signpost" for
// RESCSP01.
//
// Object class RESARE is symbolised for the effect of attribute RESTRN in a separate
// conditional symbology procedure called RESARE02.
//
// Since many of the areas concerned cover shipping channels, the number of symbols used
// is minimised to reduce clutter. To do this, values of RESTRN are ranked for significance
// as follows:
// "Traffic Restriction" values of RESTRN:
// (1) RESTRN 7,8: entry prohibited or restricted
//     RESTRN 14: IMO designated "area to be avoided" part of a TSS
// (2) RESTRN 1,2: anchoring prohibited or restricted
// (3) RESTRN 3,4,5,6: fishing or trawling prohibited or restricted
// (4) "Other Restriction" values of RESTRN are:
//     RESTRN 9, 10: dredging prohibited or restricted,
//     RESTRN 11,12: diving prohibited or restricted,
//     RESTRN 13   : no wake area.
{

    GString *restrn01    = NULL;
    GString *restrn01str = S57_getAttVal(geo, "RESTRN");

    if (NULL != restrn01str) {
        restrn01 = _RESCSP01(restrn01str);
    } //else
      //  restrn01 = g_string_new(";OP(----)");  // return NOOP to silence DEBUG msg

    return restrn01;
}

static GString *_RESCSP01(GString *restrnstr)
// FIXME: pass GString *restrnstr
// Remarks: See caller: RESTRN01, DEPARE01
{
    GString *rescsp01         = NULL;
    char        restrn[LISTSIZE] = {'\0'};   // restriction list
    const char *symb             = NULL;
    _parseList(restrnstr->str, restrn);

    if (_strpbrk(restrn, "\007\010\016")) {
        // continuation A
        if (_strpbrk(restrn, "\001\002\003\004\005\006"))
            symb = ";SY(ENTRES61)";
        else {
            if (_strpbrk(restrn, "\011\012\013\014\015"))
                symb = ";SY(ENTRES71)";
            else
                symb = ";SY(ENTRES51)";

        }
    } else {
        if (_strpbrk(restrn, "\001\002")) {
            // continuation B
            if (_strpbrk(restrn, "\003\004\005\006"))
                symb = ";SY(ACHRES61)";
            else {
                    if (_strpbrk(restrn, "\011\012\013\014\015"))
                        symb = ";SY(ACHRES71)";
                    else
                        symb = ";SY(ACHRES51)";
                }
        } else {
            if (_strpbrk(restrn, "\003\004\005\006")) {
                // continuation C
                if (_strpbrk(restrn, "\011\012\013\014\015"))
                    symb = ";SY(FSHRES71)";
                else
                    symb = ";SY(FSHRES51)";
            } else {
                if (_strpbrk(restrn, "\011\012\013\014\015"))
                    symb = ";SY(INFARE51)";
                else
                    symb = ";SY(RSRDEF51)";
            }
        }
    }

    rescsp01 = _g_string_new(rescsp01, symb);

    return rescsp01;
}

static GString *_SEABED01(double drval1, double drval2)
// Remarks: An area object that is part of the seabed is coloured as necessary according
// to the mariners selection of two shades, (shallow contour, safety contour,
// deep contour), or four shades (safety contour only). This requires a decision
// making process provided by this conditional symbology procedure. Note
// that this procedure is called as a sub-procedure by other conditional
// symbology procedures.
//
// Note: The requirement to show four depth shades is not mandatory. Also,
// the requirement to show the shallow pattern is not mandatory. However,
// both these features are strongly recommended.

// return: is never NULL

{
    GString *seabed01 = NULL;
    gboolean shallow  = TRUE;
    const char *arecol   = ";AC(DEPIT)";

    if (drval1 >= 0.0 && drval2 > 0.0)
        arecol  = ";AC(DEPVS)";

    if (TRUE == (int) S52_MP_get(S52_MAR_TWO_SHADES)){
        if (drval1 >= S52_MP_get(S52_MAR_SAFETY_CONTOUR)  &&
            drval2 >  S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
            arecol  = ";AC(DEPDW)";
            shallow = FALSE;
        }
    } else {
        if (drval1 >= S52_MP_get(S52_MAR_SHALLOW_CONTOUR) &&
            drval2 >  S52_MP_get(S52_MAR_SHALLOW_CONTOUR))
            arecol  = ";AC(DEPMS)";

            if (drval1 >= S52_MP_get(S52_MAR_SAFETY_CONTOUR)  &&
                drval2 >  S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
                arecol  = ";AC(DEPMD)";
                shallow = FALSE;
            }

            if (drval1 >= S52_MP_get(S52_MAR_DEEP_CONTOUR)  &&
                drval2 >  S52_MP_get(S52_MAR_DEEP_CONTOUR)) {
                arecol  = ";AC(DEPDW)";
                shallow = FALSE;
            }

    }

    seabed01 = _g_string_new(seabed01, arecol);

    if (TRUE==(int) S52_MP_get(S52_MAR_SHALLOW_PATTERN) && TRUE==shallow)
        g_string_append(seabed01, ";AP(DIAMOND1)");

    return seabed01;
}

static GString *SOUNDG02 (S57_geo *geo)
// Remarks: In S-57 soundings are elements of sounding arrays rather than individual
// objects. Thus this conditional symbology procedure examines each
// sounding of a sounding array one by one. To symbolize the depth values it
// calls the procedure SNDFRM02 which in turn translates the depth values
// into a set of symbols to be shown at the soundings position.
{
    guint   npt = 0;
    double *ppt = NULL;

    if (S57_POINT_T != S57_getObjtype(geo)) {
        PRINTF("WARNING: invalid object type (not POINT_T)\n");
        g_assert(0);
        return NULL;
    }

    if (FALSE == S57_getGeoData(geo, 0, &npt, &ppt)) {
        PRINTF("WARNING: invalid object type (not POINT_T)\n");
        g_assert(0);
        return NULL;
    }

    if (npt > 1) {
        PRINTF("ERROR: GDAL config error, SOUNDING array instead or point\n");
        g_assert(0);
        return NULL;
    }

    return _SNDFRM02(geo, ppt[2]);
}

static GString *_SNDFRM02(S57_geo *geo, double depth_value)
// Remarks: Soundings differ from plain text because they have to be readable under all
// circumstances and their digits are placed according to special rules. This
// conditional symbology procedure accesses a set of carefully designed
// sounding symbols provided by the symbol library and composes them to
// sounding labels. It symbolizes swept depth and it also symbolizes for low
// reliability as indicated by attributes QUASOU and QUAPOS.
{
    GString *sndfrm02         = _g_string_new(NULL, NULL);
    const char *symbol_prefix = NULL;
    GString *tecsoustr        = S57_getAttVal(geo, "TECSOU");
    //char     tecsou[LISTSIZE] = {'\0'};
    GString *quasoustr        = S57_getAttVal(geo, "QUASOU");
    char     quasou[LISTSIZE] = {'\0'};
    GString *statusstr        = S57_getAttVal(geo, "STATUS");
    char     status[LISTSIZE] = {'\0'};
    double   leading_digit    = 0.0;

    // debug
    //if (7.5 == depth_value) {
    //    PRINTF("7.5 found ID:%i\n", S57_getS57ID(geo));
    //    g_string_sprintfa(sndfrm02, ";SY(BRIDGE01)");
    //}


    // FIXME: test to fix the rounding error (!?)
    depth_value  += (depth_value > 0.0)? 0.01: -0.01;
    leading_digit = (int) depth_value;
    //leading_digit = floor(depth_value);

    if (depth_value <= S52_MP_get(S52_MAR_SAFETY_DEPTH))
        symbol_prefix = "SOUNDS";
    else
        symbol_prefix = "SOUNDG";

    if (NULL != tecsoustr) {
        char tecsou[LISTSIZE] = {'\0'};
        _parseList(tecsoustr->str, tecsou);
        if (_strpbrk(tecsou, "\006"))
            g_string_sprintfa(sndfrm02, ";SY(%sB1)", symbol_prefix);
    }

    if (NULL != quasoustr) _parseList(quasoustr->str, quasou);
    if (NULL != statusstr) _parseList(statusstr->str, status);

    if (_strpbrk(quasou, "\003\004\005\010\011") || _strpbrk(status, "\022"))
            g_string_sprintfa(sndfrm02, ";SY(%sC2)", symbol_prefix);
    else {
        GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
        int      quapos    = (NULL == quaposstr)? 0 : S52_atoi(quaposstr->str);

        if (NULL != quaposstr) {
            if (2 <= quapos && quapos < 10)
                g_string_sprintfa(sndfrm02, ";SY(%sC2)", symbol_prefix);
        }
    }

    // Continuation A
    if (depth_value < 10.0) {
        // can be above water (negative)
        int fraction = (int)ABS((depth_value - leading_digit)*10);

        g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)ABS(leading_digit));
        g_string_sprintfa(sndfrm02, ";SY(%s5%1i)", symbol_prefix, fraction);

        // above sea level (negative)
        if (depth_value < 0.0)
            g_string_sprintfa(sndfrm02, ";SY(%sA1)", symbol_prefix);

        return sndfrm02;
    }

    if (depth_value < 31.0) {
        double fraction = depth_value - leading_digit;

        if (fraction != 0.0) {
            fraction = fraction * 10;
            // FIXME: use modulus '%' instead of '/'  --check this at 100m too
            if (leading_digit >= 10.0) {
                g_string_sprintfa(sndfrm02, ";SY(%s2%1i)", symbol_prefix, (int)leading_digit/10);
                // remove tenth
                leading_digit -= floor(leading_digit/10.0) * 10.0;
            }

            g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)leading_digit);
            g_string_sprintfa(sndfrm02, ";SY(%s5%1i)", symbol_prefix, (int)fraction);

            return sndfrm02;
        }
    }

    // Continuation B
    depth_value = leading_digit;    // truncate to integer
    if (depth_value < 100.0) {
        double first_digit = floor(leading_digit / 10.0);
        double secnd_digit = floor(leading_digit - (first_digit * 10.0));

        g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)first_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s0%1i)", symbol_prefix, (int)secnd_digit);

        return sndfrm02;
    }

    if (depth_value < 1000.0) {
        double first_digit = floor((leading_digit) / 100.0);
        double secnd_digit = floor((leading_digit - (first_digit * 100.0)) / 10.0);
        double third_digit = floor( leading_digit - (first_digit * 100.0) - (secnd_digit * 10.0));

        g_string_sprintfa(sndfrm02, ";SY(%s2%1i)", symbol_prefix, (int)first_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)secnd_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s0%1i)", symbol_prefix, (int)third_digit);

        return sndfrm02;
    }

    if (depth_value < 10000.0) {
        double first_digit = floor((leading_digit) / 1000.0);
        double secnd_digit = floor((leading_digit - (first_digit * 1000.0)) / 100.0);
        double third_digit = floor((leading_digit - (first_digit * 1000.0) - (secnd_digit * 100.0)) / 10.0);
        double last_digit  = floor( leading_digit - (first_digit * 1000.0) - (secnd_digit * 100.0) - (third_digit * 10.0));

        g_string_sprintfa(sndfrm02, ";SY(%s2%1i)", symbol_prefix, (int)first_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)secnd_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s0%1i)", symbol_prefix, (int)third_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s4%1i)", symbol_prefix, (int)last_digit);

        return sndfrm02;
    }

    // Continuation C
    {
        double first_digit  = floor((leading_digit) / 10000.0);
        double secnd_digit  = floor((leading_digit - (first_digit * 10000.0)) / 1000.0);
        double third_digit  = floor((leading_digit - (first_digit * 10000.0) - (secnd_digit * 1000.0)) / 100.0);
        double fourth_digit = floor((leading_digit - (first_digit * 10000.0) - (secnd_digit * 1000.0) - (third_digit * 100.0)) / 10.0);
        double last_digit   = floor( leading_digit - (first_digit * 10000.0) - (secnd_digit * 1000.0) - (third_digit * 100.0) - (fourth_digit * 10.0));

        g_string_sprintfa(sndfrm02, ";SY(%s3%1i)", symbol_prefix, (int)first_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s2%1i)", symbol_prefix, (int)secnd_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s1%1i)", symbol_prefix, (int)third_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s0%1i)", symbol_prefix, (int)fourth_digit);
        g_string_sprintfa(sndfrm02, ";SY(%s4%1i)", symbol_prefix, (int)last_digit);

        return sndfrm02;
    }

    return sndfrm02;
}

static GString *TOPMAR01 (S57_geo *geo)
// Remarks: Topmark objects are to be symbolized through consideration of their
// platforms e.g. a buoy. Therefore this conditional symbology procedure
// searches for platforms by looking for other objects that are located at the
// same position.. Based on the finding whether the platform is rigid or
// floating, the respective upright or sloping symbol is selected and presented
// at the objects location. Buoy symbols and topmark symbols have been
// carefully designed to fit to each other when combined at the same position.
// The result is a composed symbol that looks like the traditional symbols the
// mariner is used to.
{
    // Note: This CS fall on layer 0 (NODATA) for LUPT TOPMAR SIMPLIFIED POINT and no INST
    // (hence can't land here - nothing rendered.)
    // Only LUPT TOPMAR PAPER_CHART use this CS

    GString *topshpstr = S57_getAttVal(geo, "TOPSHP");
    GString *topmar    = NULL;
    const char *sy     = NULL;

    if (NULL == topshpstr)
        sy = ";SY(QUESMRK1)";
    else {
        int floating    = FALSE; // not a floating platform
        int topshp      = (NULL==topshpstr) ? 0 : S52_atoi(topshpstr->str);

        S57_geo *other  = S57_getTouchTOPMAR(geo);

        // S-52 BUG: topmar01 differ from floating object to
        // rigid object. Since it default to rigid object, there
        // is no need to test for that!

        //if (TRUE == _atPtPos(geo, FLOATLIST))
        if (NULL != other)
            floating = TRUE;
        //else
            // FIXME: this test is wierd since it doesn't affect 'floating'
            //if (TRUE == _atPtPos(geo, RIGIDLIST))
            //    floating = FALSE;


        if (floating) {
            // floating platform
            switch (topshp) {
                case 1 : sy = ";SY(TOPMAR02)"; break;
                case 2 : sy = ";SY(TOPMAR04)"; break;
                case 3 : sy = ";SY(TOPMAR10)"; break;
                case 4 : sy = ";SY(TOPMAR12)"; break;

                case 5 : sy = ";SY(TOPMAR13)"; break;
                case 6 : sy = ";SY(TOPMAR14)"; break;
                case 7 : sy = ";SY(TOPMAR65)"; break;
                case 8 : sy = ";SY(TOPMAR17)"; break;

                case 9 : sy = ";SY(TOPMAR16)"; break;
                case 10: sy = ";SY(TOPMAR08)"; break;
                case 11: sy = ";SY(TOPMAR07)"; break;
                case 12: sy = ";SY(TOPMAR14)"; break;

                case 13: sy = ";SY(TOPMAR05)"; break;
                case 14: sy = ";SY(TOPMAR06)"; break;
                case 17: sy = ";SY(TMARDEF2)"; break;
                case 18: sy = ";SY(TOPMAR10)"; break;

                case 19: sy = ";SY(TOPMAR13)"; break;
                case 20: sy = ";SY(TOPMAR14)"; break;
                case 21: sy = ";SY(TOPMAR13)"; break;
                case 22: sy = ";SY(TOPMAR14)"; break;

                case 23: sy = ";SY(TOPMAR14)"; break;
                case 24: sy = ";SY(TOPMAR02)"; break;
                case 25: sy = ";SY(TOPMAR04)"; break;
                case 26: sy = ";SY(TOPMAR10)"; break;

                case 27: sy = ";SY(TOPMAR17)"; break;
                case 28: sy = ";SY(TOPMAR18)"; break;
                case 29: sy = ";SY(TOPMAR02)"; break;
                case 30: sy = ";SY(TOPMAR17)"; break;

                case 31: sy = ";SY(TOPMAR14)"; break;
                case 32: sy = ";SY(TOPMAR10)"; break;
                case 33: sy = ";SY(TMARDEF2)"; break;
                default: sy = ";SY(TMARDEF2)"; break;
            }
        } else {
            // not a floating platform
            switch (topshp) {
                case 1 : sy = ";SY(TOPMAR22)"; break;
                case 2 : sy = ";SY(TOPMAR24)"; break;
                case 3 : sy = ";SY(TOPMAR30)"; break;
                case 4 : sy = ";SY(TOPMAR32)"; break;

                case 5 : sy = ";SY(TOPMAR33)"; break;
                case 6 : sy = ";SY(TOPMAR34)"; break;
                case 7 : sy = ";SY(TOPMAR85)"; break;
                case 8 : sy = ";SY(TOPMAR86)"; break;

                case 9 : sy = ";SY(TOPMAR36)"; break;
                case 10: sy = ";SY(TOPMAR28)"; break;
                case 11: sy = ";SY(TOPMAR27)"; break;
                case 12: sy = ";SY(TOPMAR14)"; break;

                case 13: sy = ";SY(TOPMAR25)"; break;
                case 14: sy = ";SY(TOPMAR26)"; break;
                case 15: sy = ";SY(TOPMAR88)"; break;
                case 16: sy = ";SY(TOPMAR87)"; break;

                case 17: sy = ";SY(TMARDEF1)"; break;
                case 18: sy = ";SY(TOPMAR30)"; break;
                case 19: sy = ";SY(TOPMAR33)"; break;
                case 20: sy = ";SY(TOPMAR34)"; break;

                case 21: sy = ";SY(TOPMAR33)"; break;
                case 22: sy = ";SY(TOPMAR34)"; break;
                case 23: sy = ";SY(TOPMAR34)"; break;
                case 24: sy = ";SY(TOPMAR22)"; break;

                case 25: sy = ";SY(TOPMAR24)"; break;
                case 26: sy = ";SY(TOPMAR30)"; break;
                case 27: sy = ";SY(TOPMAR86)"; break;
                case 28: sy = ";SY(TOPMAR89)"; break;

                case 29: sy = ";SY(TOPMAR22)"; break;
                case 30: sy = ";SY(TOPMAR86)"; break;
                case 31: sy = ";SY(TOPMAR14)"; break;
                case 32: sy = ";SY(TOPMAR30)"; break;
                case 33: sy = ";SY(TMARDEF1)"; break;
                default: sy = ";SY(TMARDEF1)"; break;
            }
        }

    }

    topmar = _g_string_new(topmar, sy);

    return topmar;
}

static GString *_UDWHAZ03(S57_geo *geo, double depth_value)
// Remarks: Obstructions or isolated underwater dangers of depths less than the safety
// contour which lie within the safe waters defined by the safety contour are
// to be presented by a specific isolated danger symbol as hazardous objects
// and put in IMO category DISPLAYBASE (see (3), App.2, 1.3). This task
// is performed by this conditional symbology procedure.
{
    // Note: will set scamin to INFINITY if SY(ISODGR01) is set

    GString *udwhaz03 = NULL;
    int      danger   = FALSE;

    // first reset trigger scamin
    S57_setScamin(geo, S57_RESET_SCAMIN);

    if (depth_value <= S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
        S57_geo *geoTouch = S57_getTouchUDWHAZ(geo);
        if (NULL == geoTouch) {
            PRINTF("DEBUG: NULL geo _UDWHAZ03/getTouchDEPARE - case where depth_value < S52_MAR_SAFETY_CONTOUR\n");

            // debug
            //S57_setHighlight(geo, TRUE);
            //S57_setScamin(geo, INFINITY);

            // no danger - no need to process further - bailout
            return NULL;
        }

        // DEPARE:L
        if (S57_LINES_T == S57_getObjtype(geoTouch)) {
            GString *drval2str = S57_getAttVal(geoTouch, "DRVAL2");
            if (NULL == drval2str)
                return NULL;

            double drval2 = S52_atof(drval2str->str);

            // adjuste datum
            drval2 += S52_MP_get(S52_MAR_DATUM_OFFSET);

            if (drval2 > S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
                danger = TRUE;
            }

        } else {
            // area DEPARE:A or DRGARE:A
            GString *drval1str = S57_getAttVal(geoTouch, "DRVAL1");
            if (NULL == drval1str)
                return NULL;

            double drval1 = S52_atof(drval1str->str);

            // adjuste datum
            drval1 += S52_MP_get(S52_MAR_DATUM_OFFSET);

            if (drval1 >= S52_MP_get(S52_MAR_SAFETY_CONTOUR)) {
                danger = TRUE;
            }
        }
    } else {
        return NULL;  // no danger
    }

    /* original udwhaz03 code from specs in pslb03_2.pdf
    if (TRUE == danger) {
        GString *watlevstr = S57_getAttVal(geo, "WATLEV");
        if (NULL != watlevstr && ('1' == *watlevstr->str || '2' == *watlevstr->str))
            udwhaz03str = g_string_new(";OP(--D14050");
        else {
            udwhaz03str = g_string_new(";OP(8OD14010);SY(ISODGR01)");
            S57_setAtt(geo, "SCAMIN", "INFINITY");
        }
    }
    */

    if (TRUE == danger) {
        GString *watlevstr = S57_getAttVal(geo, "WATLEV");
        if (NULL != watlevstr && ('1' == *watlevstr->str || '2' == *watlevstr->str)) {
            udwhaz03 = _g_string_new(udwhaz03, ";OP(--D14050)");
        } else {
            // Note: UDWHAZ04 --> stay on original (OTHER!) disp cat
            //      FIXME: perhapse add PLIB rule to link to UDWHAZ04
            //             or add S52_MAR_SPECS_ED_NO:
            //            - 3.1: C1 DAI file
            //            - 3.2: this code
            //            - 3.x: specs no public
            //            - 4.0: set OP to OTHER in UDWHAZ04
            //     DRVAL 1/2 not used in UDWHAZ04 so this hack might not work

            // debug - try to find spurious
            // FIX: logically an Isolated Danger Sym (ISODGR01) would be on a POINT_T !
            // BUT: UDWHAZ03 apply to point,area. While UDWHAZ04 apply to point,line,area!!
            //if (S57_POINT_T == S57_getObjtype(geo)) {                      // fix: udwhaz03 - place ISODRG on point only
            if (S57_LINES_T != S57_getObjtype(geo)) {                      // fix: udwhaz03 - place ISODRG on point & area
                udwhaz03 = _g_string_new(udwhaz03, ";OP(8OD14010);SY(ISODGR01)");  // udwhaz04 - place ISODGR on original disp cat!
            } else {
                udwhaz03 = _g_string_new(udwhaz03, ";OP(8O-14010)");  // udwhaz04 - stay at original disp cat
            }
            S57_setScamin(geo, INFINITY);
        }

        // debug - danger = true, so must be allway visible !?!
        //S57_setScamin(geo, INFINITY);
    }

    return udwhaz03;
}

static GString *VESSEL01 (S57_geo *geo)
// Remarks: The mariner should be prompted to select from the following options:
// - ARPA target or AIS report (overall decision or vessel by vessel) (vesrce)
// - *time-period determining vector-length for all vectors (vecper)
// - whether to show a vector (overall or vessel by vessel) (vestat)
// - *whether to symbolize vector stabilization (vecstb)
// - *whether to show one-minute or six-minute vector time marks (vecmrk)
// - whether to show heading line on AIS vessel reports (headng)
// * Note that the same vector parameters should be used for own-ship and all vessel
// vectors.
{
    GString *vessel01  = _g_string_new(NULL, NULL);
    GString *vesrcestr = S57_getAttVal(geo, "vesrce");  // vessel report source
    GString *vlabelstr = S57_getAttVal(geo, "_vessel_label");

    // experimental: text label
    if (NULL != vlabelstr) {
#ifdef S52_USE_SYM_VESSEL_DNGHL
        /* experimental: close quarter - red
        GString *vestatstr = S57_getAttVal(geo, "vestat");
        if (NULL!=vestatstr && '3'==*vestatstr->str) {
            S57_setHighlight(geo, TRUE);
            //g_string_append(vessel01, ";TX(_vessel_label,3,3,3,'15110',1,1,DNGHL,76)"          );
            //g_string_append(vessel01, ";TE('%03.0lf deg','cogcrs',3,3,3,'15109',1,2,DNGHL,77)" );
            //g_string_append(vessel01, ";TE('%3.1lf kts','sogspd',3,3,3,'15109',5,2,DNGHL,78)"  );
        } else {
            S57_setHighlight(geo, FALSE);
        }
        */
#endif
        {
            g_string_append(vessel01, ";TX(_vessel_label,3,3,3,'15110',1,1,ARPAT,76)"          );
            g_string_append(vessel01, ";TE('%03.0lf deg','cogcrs',3,3,3,'15109',1,2,ARPAT,77)" );
            g_string_append(vessel01, ";TE('%3.1lf kts','sogspd',3,3,3,'15109',5,2,ARPAT,78)"  );
        }
    }

#ifdef S52_USE_SYM_AISSEL01
    // experimental: put seclected symbol on taget
    g_string_append(vessel01, ";SY(AISSEL01)");
#endif

    // add the symbols ground and water arrow right now
    // and draw only the proper one when the user enter the S52_MAR_VECSTB
    // FIXME: AIS: leave a gap (size AISSIX01) at every minute mark
    g_string_append(vessel01, ";SY(VECGND21);SY(VECWTR21);LS(SOLD,2,ARPAT)");

    // experimental: AIS draw ship's silhouettte (OWNSHP05) if length > 10 mm
    //if (TRUE == S52_MP_get(S52_MAR_SHIPS_OUTLINE) && (NULL!=vesrcestr && '2'==*vesrcestr->str))
        g_string_append(vessel01, ";SY(OWNSHP05)");

    // ARPA
    if (NULL!=vesrcestr && '1'==*vesrcestr->str) {
        g_string_append(vessel01, ";SY(ARPATG01)");

#ifdef S52_USE_SYM_VESSEL_DNGHL
        // experimental: ARPA target & close quarters
        // Note: the LS() command word for ARPA trigger vector
        // but not heading line will be drawn
        //g_string_append(vessel01, ";SY(arpatg01);LS(SOLD,1,DNGHL)");
#endif

        // FIXME: move this to GL
        // add time mark (on ARPA vector)
        //if (0.0 != S52_MP_get(S52_MAR_VECMRK)) {
            // 6 min. and 1 min. symb.
            //if (1.0 == S52_MP_get(S52_MAR_VECMRK))
                g_string_append(vessel01, ";SY(ARPSIX01);SY(ARPONE01)");

            // 6 min. symb
            //if (2.0 == S52_MP_get(S52_MAR_VECMRK))
            //    g_string_append(vessel01, ";SY(ARPSIX01)");
        //}
    }

    // AIS
    if (NULL!=vesrcestr && '2'==*vesrcestr->str) {
        //GString *vestatstr = S57_getAttVal(geo, "vestat");
        //GString *headngstr = S57_getAttVal(geo, "headng");

        // 1. Option to show vessel symbol only:
        // no heading
        //if (NULL==headngstr) {
            g_string_append(vessel01, ";SY(AISDEF01)");
        //    return vessel01;
        //}

        // sleeping
        //if (NULL!=vestatstr && '2'==*vestatstr->str) {
            g_string_append(vessel01, ";SY(AISSLP01)");
            //return vessel01;
        //}

        // active
        //if (NULL!=vestatstr && '1'==*vestatstr->str) {
            g_string_append(vessel01, ";SY(AISVES01)");
        //}

#ifdef S52_USE_SYM_VESSEL_DNGHL
        // experimental: active AIS target & close quarters - aisves01 symb in PLAUX_00.DAI
        //if (NULL!=vestatstr && '3'==*vestatstr->str) {
        //    g_string_append(vessel01, ";SY(aisves01);LS(SOLD,1,DNGHL)");
        //}
#endif

        // add heading line (50 mm)
        //if (TRUE == S52_MP_get(S52_MAR_HEADNG_LINE)) {
            g_string_append(vessel01, ";LS(SOLD,1,ARPAT)");
        //}

        // FIXME: move this to GL
        // add time mark (on AIS vector)
        //if (0.0 != S52_MP_get(S52_MAR_VECMRK)) {

            // 6 min. and 1 min. symb
            //if (1.0 == S52_MP_get(S52_MAR_VECMRK))
                g_string_append(vessel01, ";SY(AISSIX01);SY(AISONE01)");

            // 6 min. symb only
            //if (2.0 == S52_MP_get(S52_MAR_VECMRK))
            //    g_string_append(vessel01, ";SY(AISSIX01)");
        //}
    }

    // VTS
    if (NULL!=vesrcestr && '3'==*vesrcestr->str) {
        // FIXME: S52 say to use 'vesrce' but this attrib can have the
        // value 3, witch has no LUP or CS !
        PRINTF("WARNING: no specfic rendering rule for VTS report (vesrce=3)\n");
        g_assert(0);
        return vessel01;
    }

    return vessel01;
}

static GString *VESSEL02 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(VESSEL02) --> CS(VESSEL01)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return VESSEL01(geo);
}

static GString *VRMEBL01 (S57_geo *geo)
// Remarks: This conditional symbology procedure symbolizes the three cases of range
// circle, bearing line and range/bearing line. VRM's and EBL's can be ship-centred
// or freely movable, and two line-styles are available
{
    GString *vrmebl01 = _g_string_new(NULL, NULL);

    // freely movable origine symb (a dot)
    GString *ownshpcenteredstr = S57_getAttVal(geo, "_setOrigin");
    if (NULL!=ownshpcenteredstr && ('Y'==*ownshpcenteredstr->str || 'I'==*ownshpcenteredstr->str))
        g_string_append(vrmebl01, ";SY(EBLVRM11)");

    // line style
    GString *normallinestylestr = S57_getAttVal(geo, "_normallinestyle");
    if (NULL!=normallinestylestr && 'Y'==*normallinestylestr->str)
        g_string_append(vrmebl01, ";LC(ERBLNA01)");
    else
        g_string_append(vrmebl01, ";LC(ERBLNB01)");

    // symb range marker
    GString *symbrngmrkstr = S57_getAttVal(geo, "_symbrngmrk");
    if (NULL!=symbrngmrkstr && 'Y'==*symbrngmrkstr->str)
        g_string_append(vrmebl01, ";SY(ERBLTIK1)");
    else
        g_string_append(vrmebl01, ";AC(CURSR)");

    // EXPERIMENTAL: add text, bearing & range
    g_string_append(vrmebl01, ";TX(_vrmebl_label,3,3,3,'15110',1,1,CURSR,77)");

    return vrmebl01;
}

static GString *VRMEBL02 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(VRMEBL02) --> CS(VRMEBL01)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return VRMEBL01(geo);
}

static GString *WRECKS02 (S57_geo *geo)
// Remarks: Wrecks of depths less than the safety contour which lie within the safe waters
// defined by the safety contour are to be presented by a specific isolated
// danger symbol and put in IMO category DISPLAYBASE (see (3), App.2,
// 1.3). This task is performed by the sub-procedure "UDWHAZ03" which is
// called by this symbology procedure.
{
    GString *wrecks02 = NULL;
    GString *sndfrm02 = NULL;
    GString *udwhaz03 = NULL;
    GString *quapnt01 = NULL;

    GString *valsoustr   = S57_getAttVal(geo, "VALSOU");
    double   valsou      = UNKNOWN_DEPTH;
    double   least_depth = UNKNOWN_DEPTH;
    double   depth_value = UNKNOWN_DEPTH;

    // debug
    // CA279037.000
    //if (6246 == S57_getS57ID(geo)) {
    //    PRINTF("WRECKS found\n");
    //}
    // CA479020.000
    //if (5620 == S57_getS57ID(geo)) {
    //    PRINTF("DEBUG: 5620 Q40 wreck \n");
    //    //g_assert(0);
    //}

    if (NULL != valsoustr) {
        valsou      = S52_atof(valsoustr->str);
        depth_value = valsou;
        sndfrm02    = _SNDFRM02(geo, depth_value);
    } else {
        if (S57_AREAS_T == S57_getObjtype(geo)) {
            least_depth = _DEPVAL01(geo, UNKNOWN_DEPTH);
        }

        if (UNKNOWN_DEPTH == least_depth) {
            // WARNING: ambiguity removed in WRECKS03 (see update in C&S_MD2.PDF)
            GString *watlevstr = S57_getAttVal(geo, "WATLEV");
            GString *catwrkstr = S57_getAttVal(geo, "CATWRK");

            if (NULL == watlevstr) // default (missing)
                depth_value = -15.0;
            else {
                switch (*watlevstr->str) { // ambiguous
                    case '1':
                    case '2': depth_value = -15.0 ; break;
                    case '3': depth_value =   0.01; break;
                    case '4': depth_value = -15.0 ; break;
                    case '5': depth_value =   0.0 ; break;
                    case '6': depth_value = -15.0 ; break;
                    //default :{
                    //     if (NULL != catwrkstr) {
                    //        switch (*catwrkstr->str) {
                    //            case '1': depth_value =  20.0; break;
                    //            case '2': depth_value =   0.0; break;
                    //            case '4':
                    //            case '5': depth_value = -15.0; break;
                    //        }
                    //    }
                    //}
                }

                if (NULL != catwrkstr) {
                    switch (*catwrkstr->str) {
                        case '1': depth_value =  20.0; break;
                        case '2': depth_value =   0.0; break;
                        case '4':
                        case '5': depth_value = -15.0; break;
                    }
                }
            }
        } else {
            depth_value = least_depth;
        }
    }

    udwhaz03 = _UDWHAZ03(geo, depth_value);
    quapnt01 = _QUAPNT01(geo);

    if (S57_POINT_T == S57_getObjtype(geo)) {
        // FIX: chenzunfeng note that if NULL!=udwhaz03 doesn't meen to show FOULAR01
        if ((NULL!=udwhaz03) && (NULL!=strstr(udwhaz03->str, "ISODGR"))) {
            wrecks02 = _g_string_new(wrecks02, udwhaz03->str);

            if (NULL != quapnt01)
                g_string_append(wrecks02, quapnt01->str);

        } else {
            // Continuation A (POINT_T)
            if (UNKNOWN_DEPTH != valsou) {

                if (valsou <= 20.0) {
                    wrecks02 = _g_string_new(wrecks02, ";SY(DANGER01)");
                    if (NULL != sndfrm02)
                        g_string_append(wrecks02, sndfrm02->str);

                } else {
                    wrecks02 = _g_string_new(wrecks02, ";SY(DANGER02)");
                }

                // !!! - check this - doesn't make sens
                if (NULL != udwhaz03)
                    g_string_append(wrecks02, udwhaz03->str);
                if (NULL != quapnt01)
                    g_string_append(wrecks02, quapnt01->str);

            } else {
                //const char *sym    = NULL;
                const char *sym    = ";SY(WRECKS05)";  // default
                GString *catwrkstr = S57_getAttVal(geo, "CATWRK");
                GString *watlevstr = S57_getAttVal(geo, "WATLEV");

                if (NULL!=catwrkstr && NULL!=watlevstr) {
                    if ('1'==*catwrkstr->str && '3'==*watlevstr->str)
                        sym =";SY(WRECKS04)";
                    else
                        if ('2'==*catwrkstr->str && '3'==*watlevstr->str)
                            sym = ";SY(WRECKS05)";
                }
                //else {
                if (NULL!=catwrkstr && ('4' == *catwrkstr->str || '5' == *catwrkstr->str)) {
                    sym = ";SY(WRECKS01)";
                }
                //else {
                        if (NULL != watlevstr) {
                            if ('1' == *watlevstr->str ||
                                '2' == *watlevstr->str ||
                                '5' == *watlevstr->str ||
                                '4' == *watlevstr->str )
                                sym = ";SY(WRECKS01)";
                        }
                        //else
                        //    sym = ";SY(WRECKS05)"; // default

                    //}
                //}

                wrecks02 = _g_string_new(wrecks02, sym);
                if (NULL != quapnt01)
                    g_string_append(wrecks02, quapnt01->str);

            }

        }


    } else {
        // Continuation B (AREAS_T)
        GString *quaposstr = S57_getAttVal(geo, "QUAPOS");
        int      quapos    = (NULL == quaposstr)? 0 : S52_atoi(quaposstr->str);
        const char *line   = NULL;

        if (2 <= quapos && quapos < 10)
            line = ";LC(LOWACC41)";
        else {
            if ( NULL != udwhaz03)
                line = ";LS(DOTT,2,CHBLK)";
            else {
                 if (UNKNOWN_DEPTH != valsou){
                     if (valsou <= 20)
                         line = ";LS(DOTT,2,CHBLK)";
                     else
                         line = ";LS(DASH,2,CHBLK)";
                 } else {
                     GString  *watlevstr = S57_getAttVal(geo, "WATLEV");

                     if (NULL == watlevstr)
                         line = ";LS(DOTT,2,CSTLN)";
                     else {
                         switch (*watlevstr->str){
                             case '1':
                             case '2': line = ";LS(SOLD,2,CSTLN)"; break;
                             case '4': line = ";LS(DASH,2,CSTLN)"; break;
                             case '3':
                             case '5':

                             default : line = ";LS(DOTT,2,CSTLN)"; break;
                         }
                     }

                 }
            }
        }
        wrecks02 = _g_string_new(wrecks02, line);

        if (UNKNOWN_DEPTH != valsou) {
            if (valsou <= 20) {
                if (NULL != udwhaz03)
                    g_string_append(wrecks02, udwhaz03->str);

                if (NULL != quapnt01)
                    g_string_append(wrecks02, quapnt01->str);

                if (NULL != sndfrm02)
                    g_string_append(wrecks02, sndfrm02->str);

            } else {
                // Note: ??? same as above ???
                if (NULL != udwhaz03)
                    g_string_append(wrecks02, udwhaz03->str);

                if (NULL != quapnt01)
                    g_string_append(wrecks02, quapnt01->str);
            }
        } else {
            const char *ac     = NULL;
            GString *watlevstr = S57_getAttVal(geo, "WATLEV");

            if (NULL == watlevstr)
                ac = ";AC(DEPVS)";
            else {
                switch (*watlevstr->str) {
                    case '1':
                    case '2': ac = ";AC(CHBRN)"; break;
                    case '4': ac = ";AC(DEPIT)"; break;
                    case '5':
                    case '3':
                    default : ac = ";AC(DEPVS)"; break;
                }
            }
            g_string_append(wrecks02, ac);

            if (NULL != udwhaz03)
                g_string_append(wrecks02, udwhaz03->str);

            if (NULL != quapnt01)
                g_string_append(wrecks02, quapnt01->str);
        }
    }

    _g_string_free(sndfrm02, TRUE);
    _g_string_free(udwhaz03, TRUE);
    _g_string_free(quapnt01, TRUE);

    return wrecks02;
}

static GString *WRECKS03 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(WRECKS03) --> CS(WRECKS02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return WRECKS02(geo);
}

static GString *WRECKS04 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(WRECKS04) --> CS(WRECKS02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return WRECKS02(geo);
}

static GString *WRECKS05 (S57_geo *geo)
{
    static int silent = FALSE;

    if (FALSE == silent) {
        PRINTF("FIXME: CS(WRECKS05) --> CS(WRECKS02)\n");
        PRINTF("FIXME: (this msg will not repeat)\n");
        silent = TRUE;
    }

    return WRECKS02(geo);
}

static GString *QUESMRK1 (S57_geo *geo)
// this is a catch all, the LUP link to unknown CS
{
    GString   *err = NULL;
    S57_Obj_t  ot  = S57_getObjtype(geo);

    switch (ot) {
        case S57_POINT_T: err = _g_string_new(err, ";SY(QUESMRK1)"); break;
        case S57_LINES_T: err = _g_string_new(err, ";LC(QUESMRK1)"); break;
        case S57_AREAS_T: err = _g_string_new(err, ";AP(QUESMRK1)"); break;
        default:
            PRINTF("WARNING: unknown S57 object type for CS(QUESMRK1)\n");
    }

    return err;
}

//--------------------------------
//
// JUMP TABLE SECTION
//
//--------------------------------


// Note: CS marked '????' are stub that point to CS-1
S52_CS_condSymb S52_CS_condTable[] = {
   // name      call            Sub-Procedure
   {"CLRLIN01", CLRLIN01},   //
   {"DATCVR01", DATCVR01},   //
   {"DATCVR02", DATCVR02},   // ????
   {"DEPARE01", DEPARE01},   // _RESCSP01, _SEABED01
   {"DEPARE02", DEPARE02},   // ????
   {"DEPARE03", DEPARE03},   // PLib 4.0 draft: _RESTRN03, _SEABED01, _SAFCON01
   {"DEPCNT02", DEPCNT02},   //
   {"DEPCNT03", DEPCNT03},   // PLib 4.0 draft: _SAFCON02
   {"LEGLIN02", LEGLIN02},   //
   {"LEGLIN03", LEGLIN03},   // ????
   {"LIGHTS05", LIGHTS05},   // _LITDSN01
   {"LIGHTS06", LIGHTS06},   // PLib 4.0 draft:_LITDSN01
   {"OBSTRN04", OBSTRN04},   // _DEPVAL01, _QUAPNT01, _SNDFRM02, _UDWHAZ03
   {"OBSTRN05", OBSTRN05},   // ????
   {"OBSTRN06", OBSTRN06},   // ????
   //{"OBSTRN07", OBSTRN07},   // PLib 4.0 draft:
   {"OWNSHP02", OWNSHP02},   //
   {"PASTRK01", PASTRK01},   //
   {"QUAPOS01", QUAPOS01},   // PLib 4.0 draft: _QUALIN01, _QUAPNT02
   {"RESARE02", RESARE02},   //
   {"RESARE03", RESARE03},   // ????
   //{"RESARE04", RESARE04},   // PLib 4.0 draft:
   {"RESTRN01", RESTRN01},   // PLib 4.0 draft: _RESCSP01
   {"SLCONS03", SLCONS03},   //
   //{"SLCONS04", SLCONS04},   // PLib 4.0 draft:
   {"SOUNDG02", SOUNDG02},   // _SNDFRM02
   //{"SOUNDG03", SOUNDG03},   // PLib 4.0 draft:
   {"TOPMAR01", TOPMAR01},   // PLib 4.0 draft:
   {"VESSEL01", VESSEL01},   //
   {"VESSEL02", VESSEL02},   // ????
   {"VRMEBL01", VRMEBL01},   //
   {"VRMEBL02", VRMEBL02},   // ????
   {"WRECKS02", WRECKS02},   // _DEPVAL01, _QUAPNT01, _SNDFRM02, _UDWHAZ03
   {"WRECKS03", WRECKS03},   // ????
   {"WRECKS04", WRECKS04},   // ????
   {"WRECKS05", WRECKS05},   // PLib 4.0 draft: _DEPVAL02 _QUAPNT02 _SNDFRM03 _UDWHAZ05
   {"QUESMRK1", QUESMRK1},
   {"########", NULL}
};


#if 0
//*
Mariner Parameter           used in CS (via CS)

S52_MAR_DEEP_CONTOUR        _SEABED01(via DEPARE01);
S52_MAR_SAFETY_CONTOUR      DEPCNT02; _SEABED01(via DEPARE01); _UDWHAZ03(via OBSTRN04, WRECKS02);
S52_MAR_SAFETY_DEPTH        _SNDFRM02(via OBSTRN04, WRECKS02);
S52_MAR_SHALLOW_CONTOUR     _SEABED01(via DEPARE01);
S52_MAR_SHALLOW_PATTERN     _SEABED01(via DEPARE01);
S52_MAR_SYMBOLIZED_BND      RESARE02;
S52_MAR_TWO_SHADES          _SEABED01(via DEPARE01);

// not implemented
S52_MAR_DISTANCE_TAGS       LEGLIN02;
S52_MAR_TIME_TAGS           ?


//CS          called by S57 objects
DEPARE01  <-  DEPARE DRGARE
DEPARE02  <-  ????
DEPARE03  <-  DEPARE DRGARE

DEPCNT02  <-  DEPARE DEPCNT
DEPCNT03  <-  DEPARE DEPCNT

LIGHTS05  <-  LIGHTS
OBSTRN04  <-  OBSTRN UWTROC
RESARE02  <-  RESARE
WRECKS02  <-  WRECKS


//-----------------------------------
S57_getTouch() is called by:

DEPCNT02
_DEPVAL01 <-  OBSTRN04, WRECKS02
LIGHTS05
TOPMAR01
_UDWHAZ03 <-  OBSTRN04, WRECKS02


PLib 4.0 draft CS   PLib 3.1
DEPVAL02               _DEPVAL01
LITDSN02               _LITDSN01
RESCSP02               _RESCSP01
SAFCON01               _SAFCON01
SEABED01               _SEABED01
SNDFRM04               _SNDFRM03
UDWHAZ05               _UDWHAZ05

//   {"SYMINS02", SYMINS02},   // PLib 4.0 draft:

 PLib 4.0 draft: share Sub-Procedure

S-57 Object(Geometry) CSP name    Sub-Procedure name
---------------------------------------------------------------------

DEPARE(a)             DEPAREnn    RESCSPnn SEABEDnn SAFCONnn
DRGARE(a)

DEPARE(l)             DEPCNTnn    SAFCONnn
DEPCNT(l)

LIGHTS(p)             LIGHTS06    LITDSNnn

OBSTRN(pla)           OBSTRN07    DEPVALnn QUAPNTnn SNDFRMnn UDWHAZnn
UWTROC(p)

LNDARE(pl)            QUAPOS01    QUAPNTnn QUALINnn
COALNE(l)

RESARE(a)             RESAREnn

ACHARE(a)             RESTRNnn    RESCSPnn
CBLARE(a)
DMPGRD(a)
DWRTPT(a)
FAIRWY(a)
ICNARE(a)
ISTZNE(a)
MARCUL(a)
MIPARE(a)
OSPARE(a)
PIPARE(a)
PRCARE(a)
SPLARE(a)
SUBTLN(a)
TESARE(a)
TSSCRS(a)
TSSLPT(a)
TSSRON(a)

SOUNDG(p)            SOUNDGnn     SNDFRMnn

WRECKS(pa)           WRECKSnn     DEPVALnn QUAPNTnn SNDFRMnn UDWHAZnn


----------------------------------------------------------------------
Attribute name used by S57_getAttVal(geo, ..);
$ grep S57_getAttVal S52CS.c| sed -e 's/.*\"\(.*\)\");/\1/' > CS_att.txt
$ edit CS_att.txt  (clean up)
$ sort CS_att.txt | uniq > CS_att2.txt

catclr
CATCOV
CATLIT
CATOBS
catpst
CATREA
CATSLC
CATWRK
COLOUR
CONDTN
CONRAD
DRVAL1
DRVAL2
_extend_arc_radius
FIDN
headng
HEIGHT
INTU
LITCHR
LITVIS
LNAM
_normallinestyle
OBJL
ORIENT
plnspd
QUAPOS
QUASOU
RESTRN
SECTR1
SECTR2
select
_setOrigin
SIGGRP
SIGPER
STATUS
_symbrngmrk
TECSOU
TOPSHP
VALDCO
VALNMR
VALSOU
vesrce
_vessel_label
vestat
WATLEV


//*/
#endif  // 0
