/** @file vehicle_android.c
 * @brief android uses dbus signals
 *
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2008 Navit Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 * @Author Tim Niemeyer <reddog@mastersword.de>
 * @date 2008-2009
 */

#include <config.h>
#include <string.h>
#include <glib.h>
#include <math.h>
#include <time.h>
#include "debug.h"
#include "callback.h"
#include "plugin.h"
#include "coord.h"
#include "item.h"
#include "android.h"
#include "vehicle.h"

struct vehicle_priv {
	struct callback_list *cbl;
	struct coord_geo geo;      /**< The last known position of the vehicle **/
	double speed;              /**< Speed in km/h **/
	double direction;          /**< Bearing in degrees **/
	double height;             /**< Elevation in meters **/
	double radius;             /**< Position accuracy in meters **/
	int fix_type;              /**< Type of last fix (1 = valid, 0 = invalid) **/
	time_t fix_time;           /**< Timestamp of last fix (not used) **/
	char fixiso8601[128];      /**< Timestamp of last fix in ISO 8601 format **/
	int sats;                  /**< Number of satellites in view **/
	int sats_used;             /**< Number of satellites used in fix **/
	int have_coords;           /**< Whether the vehicle coordinates in {@code geo} are valid **/
	struct attr ** attrs;
	struct callback *pcb;      /**< The callback function for position updates **/
	struct callback *scb;      /**< The callback function for status updates **/
	struct callback *fcb;      /**< The callback function for fix status updates **/
	jclass NavitVehicleClass;  /**< The {@code NavitVehicle} class **/
	jobject NavitVehicle;      /**< An instance of {@code NavitVehicle} **/
	jclass LocationClass;      /**< Android's {@code Location} class **/
	jmethodID Location_getLatitude, Location_getLongitude, Location_getSpeed, Location_getBearing, Location_getAltitude, Location_getTime, Location_getAccuracy;
};

/**
 * @brief Free the android_vehicle
 * 
 * @param priv vehicle_priv structure for the vehicle
 * @returns nothing
 */
static void
vehicle_android_destroy(struct vehicle_priv *priv)
{
	dbg(0,"enter\n");
	g_free(priv);
}

/**
 * @brief Retrieves a vehicle attribute.
 *
 * @param priv vehicle_priv structure for the vehicle
 * @param type The attribute type to retrieve
 * @param attr Points to an attr structure that will receive the attribute data
 * @returns True for success, false for failure
 */
static int
vehicle_android_position_attr_get(struct vehicle_priv *priv,
			       enum attr_type type, struct attr *attr)
{
	dbg(1,"enter %s\n",attr_to_name(type));
	switch (type) {
	case attr_position_fix_type:
		attr->u.num = priv->fix_type;
		break;
	case attr_position_height:
		attr->u.numd = &priv->height;
		break;
	case attr_position_speed:
		attr->u.numd = &priv->speed;
		break;
	case attr_position_direction:
		attr->u.numd = &priv->direction;
		break;
	case attr_position_radius:
		attr->u.numd = &priv->radius;
		break;
	case attr_position_qual:
		attr->u.num = priv->sats;
		break;
	case attr_position_sats_used:
		attr->u.num = priv->sats_used;
		break;
	case attr_position_coord_geo:
		attr->u.coord_geo = &priv->geo;
		if (!priv->have_coords)
			return 0;
		break;
	case attr_position_time_iso8601:
		attr->u.str=priv->fixiso8601;
		break;
	case attr_position_valid:
		attr->u.num = priv->have_coords ? attr_position_valid_valid : attr_position_valid_invalid;
		break;
	default:
		return 0;
	}
	dbg(1,"ok\n");
	attr->type = type;
	return 1;
}

struct vehicle_methods vehicle_android_methods = {
	vehicle_android_destroy,
	vehicle_android_position_attr_get,
};

/**
 * @brief Called when a new position has been reported
 *
 * This function is called by {@code NavitLocationListener} upon receiving a new {@code Location}.
 *
 * @param v The {@code struct_vehicle_priv} for the vehicle
 * @param location A {@code Location} object describing the new position
 */
static void
vehicle_android_position_callback(struct vehicle_priv *v, jobject location) {
	time_t tnow;
	struct tm *tm;
	dbg(1,"enter\n");

	v->geo.lat = (*jnienv)->CallDoubleMethod(jnienv, location, v->Location_getLatitude);
	v->geo.lng = (*jnienv)->CallDoubleMethod(jnienv, location, v->Location_getLongitude);
	v->speed = (*jnienv)->CallFloatMethod(jnienv, location, v->Location_getSpeed)*3.6;
	v->direction = (*jnienv)->CallFloatMethod(jnienv, location, v->Location_getBearing);
	v->height = (*jnienv)->CallDoubleMethod(jnienv, location, v->Location_getAltitude);
	v->radius = (*jnienv)->CallFloatMethod(jnienv, location, v->Location_getAccuracy);
	tnow=(*jnienv)->CallLongMethod(jnienv, location, v->Location_getTime)/1000;
	tm = gmtime(&tnow);
	strftime(v->fixiso8601, sizeof(v->fixiso8601), "%Y-%m-%dT%TZ", tm);
	dbg(1,"lat %f lon %f time %s\n",v->geo.lat,v->geo.lng,v->fixiso8601);
	if (!v->have_coords) {
		v->have_coords=1;
		callback_list_call_attr_0(v->cbl, attr_position_valid);
	}
	callback_list_call_attr_0(v->cbl, attr_position_coord_geo);
}

/**
 * @brief Called when a new GPS status has been reported
 *
 * This function is called by {@code NavitLocationListener} upon receiving a new {@code GpsStatus}.
 *
 * @param v The {@code struct_vehicle_priv} for the vehicle
 * @param sats_in_view The number of satellites in view
 * @param sats_used The number of satellites currently used to determine the position
 */
static void
vehicle_android_status_callback(struct vehicle_priv *v, int sats_in_view, int sats_used) {
	if (v->sats != sats_in_view) {
		v->sats = sats_in_view;
		callback_list_call_attr_0(v->cbl, attr_position_qual);
	}
	if (v->sats_used != sats_used) {
		v->sats_used = sats_used;
		callback_list_call_attr_0(v->cbl, attr_position_sats_used);
	}
}

/**
 * @brief Called when a change in GPS fix status has been reported
 *
 * This function is called by {@code NavitLocationListener} upon receiving a new {@code android.location.GPS_FIX_CHANGE} broadcast.
 *
 * @param v The {@code struct_vehicle_priv} for the vehicle
 * @param fix_type The fix type (1 = valid, 0 = invalid)
 */
static void
vehicle_android_fix_callback(struct vehicle_priv *v, int fix_type) {
	if (v->fix_type != fix_type) {
		dbg(0, "fix_type changed to %d\n", fix_type); //FIXME: debug code
		v->fix_type = fix_type;
		callback_list_call_attr_0(v->cbl, attr_position_fix_type);
	}
}

/**
 * @brief Initializes an Android vehicle
 *
 * @return True on success, false on failure
 */
static int
vehicle_android_init(struct vehicle_priv *ret)
{
	jmethodID cid;

	if (!android_find_class_global("android/location/Location", &ret->LocationClass))
                return 0;
	if (!android_find_method(ret->LocationClass, "getLatitude", "()D", &ret->Location_getLatitude))
                return 0;
	if (!android_find_method(ret->LocationClass, "getLongitude", "()D", &ret->Location_getLongitude))
                return 0;
	if (!android_find_method(ret->LocationClass, "getSpeed", "()F", &ret->Location_getSpeed))
                return 0;
	if (!android_find_method(ret->LocationClass, "getBearing", "()F", &ret->Location_getBearing))
                return 0;
	if (!android_find_method(ret->LocationClass, "getAltitude", "()D", &ret->Location_getAltitude))
                return 0;
	if (!android_find_method(ret->LocationClass, "getTime", "()J", &ret->Location_getTime))
                return 0;
	if (!android_find_method(ret->LocationClass, "getAccuracy", "()F", &ret->Location_getAccuracy))
                return 0;
	if (!android_find_class_global("org/navitproject/navit/NavitVehicle", &ret->NavitVehicleClass))
                return 0;
        dbg(0,"at 3\n");
        cid = (*jnienv)->GetMethodID(jnienv, ret->NavitVehicleClass, "<init>", "(Landroid/content/Context;III)V");
        if (cid == NULL) {
                dbg(0,"no method found\n");
                return 0; /* exception thrown */
        }
        dbg(0,"at 4 android_activity=%p\n",android_activity);
        ret->NavitVehicle=(*jnienv)->NewObject(jnienv, ret->NavitVehicleClass, cid, android_activity, (int) ret->pcb, (int) ret->scb, (int) ret->fcb);
        dbg(0,"result=%p\n",ret->NavitVehicle);
	if (!ret->NavitVehicle)
		return 0;
        if (ret->NavitVehicle)
				ret->NavitVehicle = (*jnienv)->NewGlobalRef(jnienv, ret->NavitVehicle);

	return 1;
}

/**
 * @brief Create android_vehicle
 * 
 * @param meth
 * @param cbl
 * @param attrs
 * @returns vehicle_priv
 */
static struct vehicle_priv *
vehicle_android_new_android(struct vehicle_methods *meth,
	       		struct callback_list *cbl,
		       	struct attr **attrs)
{
	struct vehicle_priv *ret;

	dbg(0, "enter\n");
	ret = g_new0(struct vehicle_priv, 1);
	ret->cbl = cbl;
	ret->pcb = callback_new_1(callback_cast(vehicle_android_position_callback), ret);
	ret->scb = callback_new_1(callback_cast(vehicle_android_status_callback), ret);
	ret->fcb = callback_new_1(callback_cast(vehicle_android_fix_callback), ret);
	ret->have_coords = 0;
	ret->sats = 0;
	ret->sats_used = 0;
	*meth = vehicle_android_methods;
	vehicle_android_init(ret);
	dbg(0, "return\n");
	return ret;
}

/**
 * @brief register vehicle_android
 * 
 * @returns nothing
 */
void
plugin_init(void)
{
	dbg(0, "enter\n");
	plugin_register_vehicle_type("android", vehicle_android_new_android);
}
