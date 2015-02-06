/* some changes in navigation.c
 *
 *
 *
 * some relate to (partially or in whole)
 * #1265 from mvglaslow
 * #1271 from jandegr
 * #1274 from jandegr
 * #1174 from arnaud le meur
 * #1082 from robotaxi
 * #921 from psoding
 * #795 from user:ps333
 * #694 from user:nop
 * #660 from user:polarbear_n
 *
 * and an incomplete list of more navigation.c related tickets
 * #1190
 * #1160
 * #1161
 * #1095
 * #1087
 * #880
 * #870
 * (#519)
 *
 */
 
/* KNOWN ISSUES
 *
 *
 *  in navigation_itm_new() : If a way splits in 2, exit_to info ends up on
 *	both continuations of the ramp, sometimes leading to wrong guidance
 *	The case where a ramp itself splits in 2 is already covered by ignoring exit_to info
 *	in such cases.
 *
 *
 */

/**
 * Navit, a modular navigation system.
 * Copyright (C) 2005-2015 Navit Team
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <glib.h>
#include "debug.h"
#include "profile.h"
#include "navigation.h"
#include "coord.h"
#include "item.h"
#include "xmlconfig.h"
#include "route.h"
#include "transform.h"
#include "mapset.h"
#include "projection.h"
#include "map.h"
#include "navit.h"
#include "callback.h"
#include "speech.h"
#include "vehicleprofile.h"
#include "plugin.h"
#include "navit_nls.h"
#include "util.h"
#include "linguistics.h"

/* #define DEBUG */

static int roundabout_extra_length=50;

/* TODO: find out if this is being used elsewhere and, if so, move this definition somewhere more generic */
static int invalid_angle = 361;

/* FIXME: abandon in favor of min_turn_limit once keep left/right maneuvers are fully implemented */
static int angle_straight = 2;	/* turns with -angle_straight <= delta <= angle_straight
								 * will be seen as going straight.
								 *
								 * Use a really narrow gap here, fixes already a large number
								 * of false commands without causing other problems.
								 *
								 * During testing it became clear that widening the gap reduces
								 * even more unwanted 'go right or left' but soon starts to show side-effects.
								 *
								 * maybe think of a better name some day.
								 *
								 */

/** Minimum absolute delta for a turn.
 * Maneuvers whose absolute delta is less than this will be considered straight */
static int min_turn_limit = 25;

/* FIXME: revisit these limits. IMHO (mvglasow):
 *
 * 0 degrees = perfect straight road
 * 90 degrees = perfect turn
 * 180 degrees = perfect U turn
 *
 * Then, by interpolation:
 * 45 degrees = perfect "light turn"
 * 135 degrees = perfect "sharp turn"
 *
 * This also agrees with the angles depicted in the maneuver icons.
 *
 * Thresholds should be roughly halfway between them.
 * 25 degrees for min_turn_limit is probably OK (would be 22.5 by the above definition),
 * but maybe the rest should be somewhat closer to 67.5-117.5-157.5 instead of 45-105-165.
 */

/** Minimum absolute delta for a turn of "normal" strength (which is always just announced as "turn left/right" even when strength is required).
 * Maneuvers whose absolute delta is less than this will be announced as "turn easily left/right" when strength is required. */
static int turn_2_limit = 45;

/** Minimum absolute delta for a sharp turn.
 * Maneuvers whose absolute delta is equal to or greater than this will be announced as "turn sharply left/right" when strength is required. */
static int sharp_turn_limit = 105;

/** Minimum absolute delta for a U turn.
 * Maneuvers whose absolute delta is less than this (but at least {@code min_turn_limit}) will always be announced as turns.
 * Note that, depending on other conditions, even maneuvers whose delta exceeds the threshold may still be announced as (sharp) turns. */
static int u_turn_limit = 165;

struct suffix {
	enum gender {unknown, male, female, neutral};
	char *fullname;
	char *abbrev;
	int sex;
} suffixes[]= {
	{"weg",NULL,male},
	{"platz","pl.",male},
	{"ring",NULL,male},
	{"bogen",NULL,male},
	{"allee",NULL,female},
	{"gasse",NULL,female},
	{"straße","str.",female},

	/* some for the dutch lang. */
	{"straat",NULL,neutral},
/*	{"weg",NULL,neutral}, doubles-up with German */
	{"baan",NULL,neutral},
	{"laan",NULL,neutral},
	{"wegel",NULL,neutral},

	/* some for the english lang. */
	{"street",NULL,male},
	{"drive",NULL,male},

};


struct navigation {
	NAVIT_OBJECT
	struct route *route;
	struct map *map;
	struct item_hash *hash;
	struct vehicleprofile *vehicleprofile;
	struct navigation_itm *first;
	struct navigation_itm *last;
	struct navigation_command *cmd_first;
	struct navigation_command *cmd_last;
	struct callback_list *callback_speech;
	struct callback_list *callback;
	struct navit *navit;
	struct speech *speech;
	int level_last;
	struct item item_last;
	int turn_around;
	int turn_around_limit;
	int distance_turn;
	struct callback *route_cb;
	int announce[route_item_last-route_item_first+1][3];
	int tell_street_name;
	int delay;
	int curr_delay;
	int turn_around_count;
	int flags;
};

int distances[]={1,2,3,4,5,10,25,50,75,100,150,200,250,300,400,500,750,-1};


/* Allowed values for navigation_maneuver.merge_or_exit
 * The numeric values are chosen in such a way that they can be interpreted as flags:
 * 1=merge, 2=exit, 4=interchange, 8=right, 16=left
 * Identifiers were chosen over flags to enforce certain rules
 * (merge/exit/interchange and left/right are mutually exclusive, left/right requires merge or exit). */
//FIXME: should we make this an enum?

/** Not merging into or exiting from a motorway_like road */
#define mex_none 0

/** Merging into a motorway-like road, direction undefined */
//FIXME: do we need this constant?
#define mex_merge 1

/** Exiting from a motorway-like road, direction undefined.
 * This should only be used for ramps leading to a non-motorway road.
 * For interchanges, use {@code mex_interchange} instead. */
//FIXME: do we need this constant?
#define mex_exit 2

/** Motorway-like road splits in two.
 * This should be used for all cases in which ramps lead to another motorway-like road. */
#define mex_interchange 4

/** Merging into a motorway-like road to the right (coming from the left) */
#define mex_merge_right 9

/** Exiting from a motorway-like road to the right.
 * See {@code mex_exit} for usage. */
#define mex_exit_right 10

/** Merging into a motorway-like road to the left (coming from the right) */
#define mex_merge_left 17

/** Exiting from a motorway-like road to the left.
 * See {@code mex_exit} for usage. */
#define mex_exit_left 18

/**
 * @brief Holds information about a navigation maneuver.
 *
 * This structure is populated when a navigation maneuver is first analyzed. Its members contain all information
 * needed to decide whether or not to announce the maneuver, what type of maneuver it is and the information that
 * was used to determine the former two.
 */
struct navigation_maneuver {
	enum item_type type;       /**< The type of maneuver to perform. Any {@code nav_*} item is permitted here, with one exception:
	                                merge or exit maneuvers are indicated by the {@code merge_or_exit} member. The {@code item_type}
	                                for such maneuvers should be a turn instruction in cases where the maneuver is ambiguous, or
	                                {@code nav_none} for cases in which we would expect the driver to perform this maneuver even
	                                without being instructed to do so. **/
	int delta;                 /**< Bearing difference (the angle the driver has to steer) for the maneuver */
	int merge_or_exit;         /**< Whether we are merging into or exiting from a motorway_like road or we are at an interchange */
	int is_complex_t_junction; /**< Whether we are coming from the "stem" of a T junction whose "bar" is a dual-carriageway road and
	                                crossing the opposite lane of the "bar" first (i.e. turning left in countries that drive on the
	                                right, or turning right in countries that drive on the left). For these maneuvers
	                                {@code num_options} is 1 (which means we normally wouldn't announce the maneuver) but drivers
	                                would expect an announcement in such cases. */
	int num_options;           /**< Number of permitted candidate ways, i.e. ways which we may enter (based on access flags of the
	                                way but without considering turn restrictions). Permitted candidate ways include the route. */
	int num_new_motorways;     /**< Number of permitted candidate ways that are motorway-like */
	int num_other_ways;        /**< Number of permitted candidate ways that are neither ramps nor motorway-like */
	int old_cat;               /**< Maneuver category of the way leading to the maneuver */
	int new_cat;               /**< Maneuver category of the selected way after the maneuver */
	int max_cat;               /**< Highest maneuver category of any permitted candidate way other than the route */
	int num_similar_ways;      /**< Number of candidate ways (including the route) that have a {@code maneuver_category()} similar
	                                to {@code old_cat}. See {@code maneuver_required2()} for definition of "similar". */
	int left;                  /**< Minimum bearing delta of any candidate way left of the route, -180 for none */
	int right;                 /**< Minimum bearing delta of any candidate way right of the route, 180 for none */
	int is_unambiguous;        /**< Whether the maneuver is unambiguous. A maneuver is unambiguous if, despite
	                                multiple candidate way being available, we can reasonable expect the driver to
	                                continue on the route without being told to do so. This is typically the case when
	                                the route stays on the main road and goes straight, while all other candidate ways
	                                are minor roads and involve a significant turn. */
	int is_same_street;        /**< Whether the street keeps its name after the maneuver. */
};

/**
 * @brief Holds information about a command for a navigation maneuver.
 *
 * An instance of this structure is generated for each navigation maneuver that is to be announced.
 */
struct navigation_command {
	struct navigation_itm *itm;            /**< The navigation item following the maneuver */
	struct navigation_command *next;       /**< next command in the list */
	struct navigation_command *prev;       /**< previous command in the list */
	int delta;                             /**< bearing change at maneuver */
	int roundabout_delta;                  /**< if we are leaving a roundabout, effective bearing change (between entry and exit) with some corrections applied */
	int length;                            /**< if the maneuver is a roundabout, distance between entry and exit (plus penalty), else 0 */
	struct navigation_maneuver *maneuver;  /**< Details on the maneuver to perform */
};

/*
 * @brief Holds a way that one could possibly drive from a navigation item
 *
 */
struct navigation_way {
	struct navigation_way *next;	/**< Pointer to a linked-list of all navigation_ways from this navigation item */
	short dir;						/**< The direction -1 or 1 of the way */
	short angle2;					/**< The angle one has to steer to drive from the old item to this street */



	/*
	 * (mvglasow) angle2 might be the bearing at the start of the way (0 = north, 90 = east etc.),
	 * this needs further examination
	 *
	 */



	int flags;						/**< The flags of the way */
	struct item item;				/**< The item of the way */
	char *name;						/**< The street name ({@code street_name} attribute) */
	char *name_systematic;			/**< The road number ({@code street_name_systematic} attribute, OSM: {@code ref}) */
	char *exit_ref;					/**< Exit_ref if found on the first node of the way*/
	char *exit_label;				/**< Exit_label if found on the first node of the way*/
	struct street_destination *destination;				/**< The destination this way leads to (OSM: {@code destination}) */
};

struct navigation_itm {
	struct navigation_way way;
	int angle_end;                      /* FIXME: is this the bearing at the end of way? */
	struct coord start,end;
	int time;
	int length;
	int speed;
	int dest_time;
	int dest_length;
	int told;							/**< Indicates if this item's announcement has been told earlier and should not be told again*/
	int streetname_told;				/**< Indicates if this item's streetname has been told in speech navigation*/
	int dest_count;
	struct navigation_itm *next;
	struct navigation_itm *prev;
};


/*@brief A linked list conataining the destination of the road
 *
 *
 * Holds the destination info from the road, that is the place
 * you drive to if you keep following the road as found on
 * traffic sign's (ex. Paris, Senlis ...)
 *
 *
 */

struct street_destination {
	struct street_destination *next;
	char *destination;
	int rank;
};



static void navigation_flush(struct navigation *this_);

/**
 * @brief Calculates the delta between two angles
 * @param angle1 The first angle
 * @param angle2 The second angle
 * @return The difference between the angles: -179..-1=angle2 is left of angle1,0=same,1..179=angle2 is right of angle1,180=angle1 is opposite of angle2
 */ 


static int
angle_delta(int angle1, int angle2)
{
	int delta=angle2-angle1;
	if (delta <= -180)
		delta+=360;
	if (delta > 180)
		delta-=360;
	return delta;
}

static int
angle_median(int angle1, int angle2)
{
	int delta=angle_delta(angle1, angle2);
	int ret=angle1+delta/2;
	if (ret < 0)
		ret+=360;
	if (ret > 360)
		ret-=360;
	return ret;
}

static int
angle_opposite(int angle)
{
	return ((angle+180)%360);
}

/*@brief : frees a list as constructed with split_string_to_list()
 *
 *
 *@param : the list to be freed
 */
static void
free_list(struct street_destination *list) {

	if (list){
		struct street_destination *clist;
		while (list){
			clist = list->next;
			g_free(list->destination);
			g_free(list);
			list = clist;
		}
		list = NULL;
	}
}

/*@brief splits a string into a list, the separator to split on can
 * 	be any character, and sets their initial rank to 0
 *
 *  splits a string into a list, the separator to split on can
 * 	be any character, removes preceding white-space char's and sets
 * 	the initial rank to 0
 *
 * @param way, a navigation_way holding the list to be fill up
 * @param raw_string, a string to be splitted
 * @param sep, a char to be used as separator to split the raw_string
 * @return an integer, the number of entries in the list
 */


static int
split_string_to_list(struct navigation_way *way, char* raw_string, char sep)
{

	struct street_destination *new_street_destination = NULL;
	struct street_destination *next_street_destination_remember = NULL;
	char *pos1 = raw_string;
	char *pos2;
	int count = 0;

	free_list(way->destination); /*in case this is a retry with a different separator.*/
	dbg(lvl_debug,"raw_string=%s split with %c\n",raw_string, sep);
	if (strlen(raw_string)>0)
		{
		count = 1;
		while (pos1)
			{
				new_street_destination = g_new(struct street_destination, 1);
				new_street_destination->next = next_street_destination_remember;
				next_street_destination_remember = new_street_destination ;
				if ((pos2 = strrchr(pos1, sep)) != NULL)
				{
					*pos2 = '\0' ;
					while (isspace(pos2[1]))
						pos2++;
					new_street_destination->destination = g_strdup(pos2+1);
					new_street_destination->rank=0;
					dbg(lvl_debug,"splitted_off_string=%s\n",new_street_destination->destination);
					count++;
				}
				else
				{
					while (isspace(pos1[0]))
						pos1++;
					new_street_destination->destination = g_strdup(pos1);
					new_street_destination->rank=0;
					pos1 = NULL;
					dbg(lvl_debug,"head_of_string=%s\n",new_street_destination->destination);
				}
				way->destination = next_street_destination_remember;
			}
		}
	return count;
}



/*@brief returns the first destination with a rank higher than zero,
 * 		 returns the first one in the list if all have rank zero.
 *
 */

static struct street_destination *
get_bestranked(struct street_destination *street_destination)
{
	struct street_destination *selected_street_destination;

	selected_street_destination = street_destination;
	while (selected_street_destination)
	{
		if (selected_street_destination->rank > 0)
			return selected_street_destination;
		selected_street_destination = selected_street_destination->next;
	}
	return street_destination;
}

/*@brief Assigns a high rank to a matching destination in the next
 * 		command having destination info, and reset existing ranks to zero
 *
 *@param street destination to be given a high rank
 *@param command
 *@return success=1 if succeeded, zero otherwise
 */
static int
set_highrank(struct street_destination *street_destination, struct navigation_command *command)
{
	struct street_destination *future_street_destination;
	struct navigation_command *next_command;
	char* destination_string;
	int success = 0;
	destination_string = street_destination->destination;

	if (command->next)
	{
		next_command=command->next;
		while (next_command)
		{
			if (next_command->itm->way.destination)
				break;
			if (!next_command->next)
				break;
			next_command=next_command->next;
		}
		if (next_command->itm->way.destination)
			future_street_destination = next_command->itm->way.destination;
		else future_street_destination = NULL;
	}

	while (future_street_destination)
	{
		if ((strcmp(destination_string,future_street_destination->destination)==0))
		{
			future_street_destination->rank=99;
			success =1;
		}
		else
			future_street_destination->rank=0;
		future_street_destination=future_street_destination->next;
	}
	return success;
}


/** @brief Selects the destination-names for the next announcement from the
 *         destination-names that are registered in the following command items.
 *
 *         The aim of this function is to find the destination-name entry that has the most hits in the following
 *         command items so that the destination name has a relevance over several announcements. If there is no 'winner'
 *         the entry is selected that is at top of the destination.
 */
static char*
select_announced_destinations(struct navigation_command *current_command)
{
	struct street_destination *current_destination = NULL;  /* the list pointer of the destination_names of the current command. */
	struct street_destination *search_destination = NULL;   /* the list pointer of the destination_names of the respective search_command. */

	struct navigation_command *search_command = NULL;

	#define MAX_LOOPS 10 /* limits the number of next command items to investigate over */
	#define MAX_DESTINATIONS 10	/* limits the number of destination entries to investigate */
	int destination_count[MAX_DESTINATIONS]; /* contains the hits of identical destinations over all */
						/* investigated command items - a 'high score' of destination names */
	int destination_index = 0;
	int search_command_counter = 0;
	int i, max_hits=0, max_hit_index;

	/* search over every following command for seeking identical destination_names */
	if (current_command->itm->way.destination)
	{	/* can we investigate over the following commands? */
		if (current_command->next)
		{	/* loop over every destination of the current command, as far as there are not more than MAX_DESTINATIONS entries. */
			destination_index = 0; /* start with the first destination */
			current_destination = current_command->itm->way.destination;
			destination_count[destination_index]=0;
			while (current_destination && (destination_index < MAX_DESTINATIONS))
			{	/* initialize the search command */
				search_command = current_command->next;
				search_command_counter = 0;
				while (search_command && (search_command_counter < MAX_LOOPS))
				{
					if (search_command->itm && search_command->itm->way.destination)
					{	/* has the search command any destinations ? */
						search_destination = search_command->itm->way.destination;
						while (search_destination)
						{	/* Search this name in the destination list of the current command. */
							if (0 == strcmp(current_destination->destination, search_destination->destination))
							{	/* enter the destination_name in the investigation list*/
								destination_count[destination_index]++;
								search_destination = NULL; /* break condition */
							}
							else search_destination = search_destination->next;
						}
					}
					search_command_counter++;
					search_command = search_command->next;
				}
				destination_index++;
				destination_count[destination_index]=0;
				current_destination = current_destination->next;
			}

			/* search for the best candidate */
			max_hits = 0;
			max_hit_index = 0;
			for (i = 0; i < destination_index; i++)
			{
				if (destination_count[i] > max_hits)
				{
					max_hits = destination_count[i];
					max_hit_index = i;
				}
			}
			/* jump to the corresponding destination_name */
			current_destination =  current_command->itm->way.destination;
			for (i = 0; i < max_hit_index; i++)
			{
				current_destination = current_destination->next;
			}

			dbg(lvl_debug,"%s, max hits =%i\n",current_destination->destination,max_hits);
			set_highrank(current_destination,current_command);
		}
	}

	/* return the best candidate, if there is any.*/
	if (max_hits)
		return g_strdup(current_destination->destination);
	return g_strdup(current_destination ? get_bestranked(current_destination)->destination:NULL);
}


int
navigation_get_attr(struct navigation *this_, enum attr_type type, struct attr *attr, struct attr_iter *iter)
{
	struct map_rect *mr;
	struct item *item;
	dbg(lvl_debug,"enter %s\n", attr_to_name(type));
	switch (type) {
	case attr_map:
		attr->u.map=this_->map;
		break;
	case attr_item_type:
	case attr_length:
	case attr_navigation_speech:
	case attr_street_name:
	case attr_street_name_systematic:
	case attr_street_destination:

		mr=map_rect_new(this_->map, NULL);
		while ((item=map_rect_get_item(mr))) {
			if (item->type != type_nav_none && item->type != type_nav_position) {
				if (type == attr_item_type) 
					attr->u.item_type=item->type;
				else { 
					if (!item_attr_get(item, type, attr))
						item=NULL;
				}
				break;
			}
		}
		map_rect_destroy(mr);
		if (!item)
			return 0;
		break;
	case attr_turn_around_count:
		attr->u.num=this_->turn_around_count;
		break;
	default:
		return navit_object_get_attr((struct navit_object *)this_, type, attr, iter);
	}
	attr->type=type;
	return 1;
}

static void
navigation_set_turnaround(struct navigation *this_, int val)
{
	if (this_->turn_around_count != val) {
		struct attr attr=ATTR_INT(turn_around_count, val);
		this_->turn_around_count=val;
		navit_object_callbacks((struct navit_object *)this_, &attr);
	}
}

int
navigation_set_attr(struct navigation *this_, struct attr *attr)
{
	switch (attr->type) {
	case attr_speech:
		this_->speech=attr->u.speech;
		break;
	default:
		break;
	}
	return navit_object_set_attr((struct navit_object *)this_, attr);
}


struct navigation *
navigation_new(struct attr *parent, struct attr **attrs)
{
	int i,j;
	struct attr * attr;
	struct navigation *ret=(struct navigation *)navit_object_new(attrs, &navigation_func, sizeof(struct navigation));
	ret->hash=item_hash_new();
	ret->callback=callback_list_new();
	ret->callback_speech=callback_list_new();
	ret->level_last=4;
	ret->distance_turn=50;
	ret->turn_around_limit=3;
	ret->navit=parent->u.navit;
	ret->tell_street_name=1;

	for (j = 0 ; j <= route_item_last-route_item_first ; j++) {
		for (i = 0 ; i < 3 ; i++) {
			ret->announce[j][i]=-1;
		}
	}

	if ((attr=attr_search(attrs, NULL, attr_tell_street_name))) {
		ret->tell_street_name = attr->u.num;
	}
	if ((attr=attr_search(attrs, NULL, attr_delay))) {
		ret->delay = attr->u.num;
	}
	if ((attr=attr_search(attrs, NULL, attr_flags))) {
		ret->flags = attr->u.num;
	}
	return ret;	
}

int
navigation_set_announce(struct navigation *this_, enum item_type type, int *level)
{
	int i;
	if (type < route_item_first || type > route_item_last) {
		dbg(lvl_debug,"street type %d out of range [%d,%d]", type, route_item_first, route_item_last);
		return 0;
	}
	for (i = 0 ; i < 3 ; i++) 
		this_->announce[type-route_item_first][i]=level[i];
	return 1;
}

static int
navigation_get_announce_level(struct navigation *this_, enum item_type type, int dist)
{
	int i;

	if (type < route_item_first || type > route_item_last)
	{
		dbg(lvl_error," item outside routable range\n");
		return -1;
	}
	for (i = 0 ; i < 3 ; i++) {
		if (dist <= this_->announce[type-route_item_first][i])
			return i;
	}
	return i;
}

static int is_way_allowed(struct navigation *nav, struct navigation_way *way, int mode);

static int
navigation_get_announce_level_cmd(struct navigation *this_, struct navigation_itm *itm, struct navigation_command *cmd, int distance)
{
	int level2,level=navigation_get_announce_level(this_, itm->way.item.type, distance);
	if (this_->cmd_first->itm->prev) {
		level2=navigation_get_announce_level(this_, cmd->itm->prev->way.item.type, distance);
		if (level2 > level)
			level=level2;
	}
	return level;
}

/* 0=N,90=E */
/**
 * @brief Gets the bearing from one point to another
 *
 * @param c1 The first coordinate
 * @param c2 The second coordinate
 * @param dir The direction: if it is -1, the bearing from c2 to c1 is returned, else the bearing from c1 to c2
 *
 * @return The bearing in degrees, {@code 0 <= result < 360}.
 */
static int
road_angle(struct coord *c1, struct coord *c2, int dir)
{
	int ret=transform_get_angle_delta(c1, c2, dir);
	dbg(lvl_debug, "road_angle(0x%x,0x%x - 0x%x,0x%x)=%d\n", c1->x, c1->y, c2->x, c2->y, ret);
	return ret;
}

static const char
*get_count_str(int n) 
{
	switch (n) {
	case 0:
		/* TRANSLATORS: the following counts refer to streets */
		return _("zeroth"); /* Not sure if this exists, neither if it will ever be needed */
	case 1:
		return _("first");
	case 2:
		return _("second");
	case 3:
		return _("third");
	case 4:
		return _("fourth");
	case 5:
		return _("fifth");
	case 6:
		return _("sixth");
	default: 
		return NULL;
	}
}

static const char
*get_exit_count_str(int n) 
{
	switch (n) {
	case 0:
		/* TRANSLATORS: the following counts refer to roundabout exits */
		return _("zeroth exit"); /* Not sure if this exists, neither if it will ever be needed */
	case 1:
		return _("first exit");
	case 2:
		return _("second exit");
	case 3:
		return _("third exit");
	case 4:
		return _("fourth exit");
	case 5:
		return _("fifth exit");
	case 6:
		return _("sixth exit");
	default: 
		return NULL;
	}
}
static int
round_distance(int dist)
{
	if (dist < 100) {
		dist=(dist+5)/10;
		return dist*10;
	}
	if (dist < 250) {
		dist=(dist+13)/25;
		return dist*25;
	}
	if (dist < 500) {
		dist=(dist+25)/50;
		return dist*50;
	}
	if (dist < 1000) {
		dist=(dist+50)/100;
		return dist*100;
	}
	if (dist < 5000) {
		dist=(dist+50)/100;
		return dist*100;
	}
	if (dist < 100000) {
		dist=(dist+500)/1000;
		return dist*1000;
	}
	dist=(dist+5000)/10000;
	return dist*10000;
}

static int
round_for_vocabulary(int vocabulary, int dist, int factor)
{
	if (!(vocabulary & 256)) {
		if (factor != 1) 
			dist=(dist+factor/2)/factor;
	} else
		factor=1;
	if (!(vocabulary & 255)) {
		int i=0,d=0,m=0;
		while (distances[i] > 0) {
			if (!i || abs(distances[i]-dist) <= d) {
				d=abs(distances[i]-dist);
				m=i;
			}
			if (distances[i] > dist)
				break;
			i++;
		}
		dbg(lvl_debug,"converted %d to %d with factor %d\n",dist,distances[m],factor);	
		dist=distances[m];
	}
	return dist*factor;
}

static int
vocabulary_last(int vocabulary)
{
	int i=0;
	if (vocabulary == 65535)
		return 1000;
	while (distances[i] > 0) 
		i++;
	return distances[i-1];
}

static char *
get_distance(struct navigation *nav, int dist, enum attr_type type, int is_length)
{
	int imperial=0,vocabulary=65535;
	struct attr attr;
	
	if (type == attr_navigation_long) {
		if (is_length)
			return g_strdup_printf(_("%d m"), dist);
		else
			return g_strdup_printf(_("in %d m"), dist);
	}
	if (navit_get_attr(nav->navit, attr_imperial, &attr, NULL))
		imperial=attr.u.num;
	if (nav->speech && speech_get_attr(nav->speech, attr_vocabulary_distances, &attr, NULL))
		vocabulary=attr.u.num;
	if (imperial) {
		if (dist*FEET_PER_METER < vocabulary_last(vocabulary)) {
			dist=round_for_vocabulary(vocabulary, dist*FEET_PER_METER, 1);
			if (is_length)
				return g_strdup_printf(_("%d feet"), dist);
			else
				return g_strdup_printf(_("in %d feet"), dist);
		}
	} else {
		if (dist < vocabulary_last(vocabulary)) {
			dist=round_for_vocabulary(vocabulary, dist, 1);
			if (is_length)
				return g_strdup_printf(_("%d meters"), dist);
			else
				return g_strdup_printf(_("in %d meters"), dist);
		}
	}
	if (imperial)
		dist=round_for_vocabulary(vocabulary, dist*FEET_PER_METER*1000/FEET_PER_MILE, 1000);
	else
		dist=round_for_vocabulary(vocabulary, dist, 1000);
	if (dist < 5000) {
		int rem=(dist/100)%10;
		if (rem) {
			if (imperial) {
				if (is_length)
					return g_strdup_printf(_("%d.%d miles"), dist/1000, rem);
				else
					return g_strdup_printf(_("in %d.%d miles"), dist/1000, rem);
			} else {
				if (is_length)
					return g_strdup_printf(_("%d.%d kilometers"), dist/1000, rem);
				else
					return g_strdup_printf(_("in %d.%d kilometers"), dist/1000, rem);
			}
		}
	}
	if (imperial) {
		if (is_length) 
			return g_strdup_printf(navit_nls_ngettext("one mile","%d miles", dist/1000), dist/1000);
		else
			return g_strdup_printf(navit_nls_ngettext("in one mile","in %d miles", dist/1000), dist/1000);
	} else {
		if (is_length) 
			return g_strdup_printf(navit_nls_ngettext("one kilometer","%d kilometers", dist/1000), dist/1000);
		else
			return g_strdup_printf(navit_nls_ngettext("in one kilometer","in %d kilometers", dist/1000), dist/1000);
	}
}


/**
 * @brief Initializes a navigation_way
 *
 * This function analyzes the underlying map item and sets the entry bearing, names and flags for the way.
 *
 * Note that entry bearing is expressed as bearing towards the opposite end of the item.
 *
 * Note that this function is not suitable for ways on the route (created in {@code navigation_itm_new})
 * as it may return incorrect coordinates for these ways.
 *
 * @param w The way to initialize. The {@code item}, {@code id_hi}, {@code id_lo} and {@code dir}
 * members of this struct must be set prior to calling this function.
 */
static void
navigation_way_init(struct navigation_way *w)
{
	struct coord cbuf[2];
	struct item *realitem;
	struct coord c;
	struct map_rect *mr;
	struct attr attr;

	w->angle2 = invalid_angle;
	mr = map_rect_new(w->item.map, NULL);
	if (!mr)
		return;

	realitem = map_rect_get_item_byid(mr, w->item.id_hi, w->item.id_lo);
	if (!realitem) {
		dbg(lvl_warning,"Item from segment not found on map!\n");
		map_rect_destroy(mr);
		return;
	}

	if (realitem->type < type_line || realitem->type >= type_area) {
		map_rect_destroy(mr);
		return;
	}
	if (item_attr_get(realitem, attr_flags, &attr))
		w->flags=attr.u.num;
	else
		w->flags=0;
	if (item_attr_get(realitem, attr_street_name, &attr))
		w->name=map_convert_string(realitem->map,attr.u.str);
	else
		w->name=NULL;
	if (item_attr_get(realitem, attr_street_name_systematic, &attr))
		w->name_systematic=map_convert_string(realitem->map,attr.u.str);
	else
		w->name_systematic=NULL;
		
	if (w->dir < 0) {
		if (item_coord_get(realitem, cbuf, 2) != 2) {
			dbg(lvl_warning,"Using calculate_angle() with a less-than-two-coords-item?\n");
			map_rect_destroy(mr);
			return;
		}
			
		while (item_coord_get(realitem, &c, 1)) {
			cbuf[0] = cbuf[1];
			cbuf[1] = c;
		}
		
	} else {
		if (item_coord_get(realitem, cbuf, 2) != 2) {
			dbg(lvl_warning,"Using calculate_angle() with a less-than-two-coords-item?\n");
			map_rect_destroy(mr);
			return;
		}
		c = cbuf[0];
		cbuf[0] = cbuf[1];
		cbuf[1] = c;
	}

	map_rect_destroy(mr);

	w->angle2=road_angle(&cbuf[1],&cbuf[0],0);
}


/**
 * @brief Returns the bearing at the end of a way
 *
 * @param w The way to examine
 *
 * @return The bearing, {@code 0 <= bearing < 360}.
 */
static int
navigation_way_get_exit_angle(struct navigation_way *w) {
	int ret = invalid_angle;
	struct coord cbuf[2];
	struct item *realitem;
	struct coord c;
	struct map_rect *mr;

	mr = map_rect_new(w->item.map, NULL);
	if (!mr)
		return ret;

	realitem = map_rect_get_item_byid(mr, w->item.id_hi, w->item.id_lo);
	if (!realitem) {
		dbg(lvl_warning,"Item from segment not found on map!\n");
		map_rect_destroy(mr);
		return ret;
	}

	if (realitem->type < type_line || realitem->type >= type_area) {
		map_rect_destroy(mr);
		return ret;
	}

	if (w->dir < 0) {
		if (item_coord_get(realitem, cbuf, 2) != 2) {
			dbg(lvl_warning,"Using calculate_angle() with a less-than-two-coords-item?\n");
			map_rect_destroy(mr);
			return ret;
		}
		c = cbuf[0];
		cbuf[0] = cbuf[1];
		cbuf[1] = c;
	}
	else {
		if (item_coord_get(realitem, cbuf, 2) != 2) {
			dbg(lvl_warning,"Using calculate_angle() with a less-than-two-coords-item?\n");
			map_rect_destroy(mr);
			return ret;
		}

		while (item_coord_get(realitem, &c, 1)) {
			cbuf[0] = cbuf[1];
			cbuf[1] = c;
		}
	}

	map_rect_destroy(mr);

	ret = road_angle(&cbuf[1],&cbuf[0],0);

	return ret;
}


/**
 * @brief Returns the bearing of a way at a given distance from its start
 *
 * {@code invalid_angle} will be returned if one of the following errors occurs:
 * <ul>
 * <li>The item is not found on the map</li>
 * <li>The item is not of the correct type</li>
 * <li>The item is shorter than {@code distance}</li>
 * </ul>
 *
 * @param pro The projection used by the map
 * @param w The way to examine
 * @param dist The distance from the start of the way at which to determine bearing
 *
 * @return The bearing, {@code 0 <= bearing < 360}, or {@code invalid_angle} if an error occurred.
 */
static int
navigation_way_get_angle_at(struct navigation_way *w, enum projection pro, double dist) {
	double dist_left = dist; /* distance from last examined point */
	int ret = invalid_angle;
	struct coord cbuf[2];
	struct item *realitem;
	struct coord c;
	struct map_rect *mr;

	mr = map_rect_new(w->item.map, NULL);
	if (!mr)
		return ret;

	realitem = map_rect_get_item_byid(mr, w->item.id_hi, w->item.id_lo);
	if (!realitem) {
		dbg(lvl_warning,"Item from segment not found on map!\n");
		map_rect_destroy(mr);
		return ret;
	}

	if (realitem->type < type_line || realitem->type >= type_area) {
		map_rect_destroy(mr);
		return ret;
	}

	if (item_coord_get(realitem, &cbuf[1], 1) != 1) {
		dbg(lvl_warning,"item has no coords\n");
		map_rect_destroy(mr);
		return ret;
	}

	if (w->dir < 0) {
		/* we're going against the direction of the item:
		 * measure its total length and set distance_left to difference of total length and distance */
		dist_left = 0;
		while (item_coord_get(realitem, &c, 1)) {
			cbuf[0] = cbuf[1];
			cbuf[1] = c;
			dist_left += transform_distance(pro, &cbuf[0], &cbuf[1]);
		}

		// FIXME: dist_left is now the complete length - subtract dist

		item_coord_rewind(realitem);

		if (item_coord_get(realitem, &cbuf[1], 1) != 1) {
			dbg(lvl_warning,"item has no more coords after rewind\n");
			map_rect_destroy(mr);
			return ret;
		}
	}

	while (item_coord_get(realitem, &c, 1)) {
		cbuf[0] = cbuf[1];
		cbuf[1] = c;
		dist_left -= transform_distance(pro, &cbuf[0], &cbuf[1]);
		if (dist_left <= 0) {
			ret = road_angle(&cbuf[0], &cbuf[1], w->dir);
			map_rect_destroy(mr);
			return ret;
		}
	}

	map_rect_destroy(mr);
	return ret;
}


/**
 * @brief Returns the maximum delta between a reference bearing and any segment of a given way, up to a given distance from its start
 *
 * The return value is the maximum delta (in terms of absolute value), but the sign is preserved. If the maximum delta is encountered
 * more than once but with different signs, {@code dir} controls which of the two values is returned:
 * <ul>
 * <li>+1: Return the first maximum encountered when following the direction of the route</li>
 * <li>-1: Return the first maximum encountered when going against the direction of the route</li>
 * </ul>
 *
 * {@code invalid_angle} will be returned if one of the following errors occurs:
 * <ul>
 * <li>The item is not found on the map</li>
 * <li>The item is not of the correct type</li>
 * <li>The item has fewer than 2 points</li>
 * <li>{@code dist} is zero</li>
 * </ul>
 *
 * If {@code dist} exceeds the length of the way, the entire way is examined.
 *
 * @param pro The projection used by the map
 * @param w The way to examine
 * @param angle The reference bearing
 * @param dist The distance from the start of the way at which to determine bearing
 * @param dir Controls how to handle when the same delta is encountered multiple times but with different signs
 *
 * @return The delta, {@code -180 < delta <= 180}, or {@code invalid_angle} if an error occurred.
 */
static int
navigation_way_get_max_delta(struct navigation_way *w, enum projection pro, int angle, double dist, int dir) {
	double dist_left = dist; /* distance from last examined point */
	int ret = invalid_angle;
	int tmp_delta;
	struct coord cbuf[2];
	struct item *realitem;
	struct coord c;
	struct map_rect *mr;

	mr = map_rect_new(w->item.map, NULL);
	if (!mr)
		return ret;

	realitem = map_rect_get_item_byid(mr, w->item.id_hi, w->item.id_lo);
	if (!realitem) {
		dbg(lvl_warning,"Item from segment not found on map!\n");
		map_rect_destroy(mr);
		return ret;
	}

	if (realitem->type < type_line || realitem->type >= type_area) {
		map_rect_destroy(mr);
		return ret;
	}

	if (item_coord_get(realitem, &cbuf[1], 1) != 1) {
		dbg(lvl_warning,"item has no coords\n");
		map_rect_destroy(mr);
		return ret;
	}

	if (w->dir < 0) {
		/* we're going against the direction of the item:
		 * measure its total length and set dist_left to difference of total length and distance */
		dist_left = 0;
		while (item_coord_get(realitem, &c, 1)) {
			cbuf[0] = cbuf[1];
			cbuf[1] = c;
			dist_left += transform_distance(pro, &cbuf[0], &cbuf[1]);
		}

		/* if dist is less that the distance of the way, make dist_left the distance from the other end
		 * else set it to zero (so we'll examine the whole way) */
		if (dist_left > dist)
			dist_left -= dist;
		else
			dist_left = 0;

		item_coord_rewind(realitem);

		if (item_coord_get(realitem, &cbuf[1], 1) != 1) {
			dbg(lvl_warning,"item has no more coords after rewind\n");
			map_rect_destroy(mr);
			return ret;
		}
	}

	while (item_coord_get(realitem, &c, 1)) {
		if ((w->dir > 0) && (dist_left <= 0))
			break;
		cbuf[0] = cbuf[1];
		cbuf[1] = c;
		dist_left -= transform_distance(pro, &cbuf[0], &cbuf[1]);
		if ((w->dir < 0) && (dist_left > 0))
			continue;
		tmp_delta = angle_delta(angle, road_angle(&cbuf[0], &cbuf[1], w->dir));
		if ((ret == invalid_angle) || (abs(ret) < abs(tmp_delta)) || ((abs(ret) == abs(tmp_delta)) && ((dir * w->dir) < 0)))
			ret = tmp_delta;
	}

	map_rect_destroy(mr);
	return ret;
}


/**
 * @brief Returns the time (in seconds) one will drive between two navigation items
 *
 * This function returns the time needed to drive between two items, including both of them,
 * in seconds.
 *
 * @param from The first item
 * @param to The last item
 * @return The travel time in seconds, or -1 on error
 */
static int
navigation_time(struct navigation_itm *from, struct navigation_itm *to)
{
	struct navigation_itm *cur;
	int time;

	time = 0;
	cur = from;
	while (cur) {
		time += cur->time;

		if (cur == to) {
			break;
		}
		cur = cur->next;
	}

	if (!cur) {
		return -1;
	}

	return time;
}

/**
 * @brief Clears the ways one can drive from itm
 *
 * @param itm The item that should have its ways cleared
 */
static void
navigation_itm_ways_clear(struct navigation_itm *itm)
{
	struct navigation_way *c,*n;

	c = itm->way.next;
	while (c) {
		n = c->next;
		map_convert_free(c->name);
		map_convert_free(c->name_systematic);
		g_free(c);
		c = n;
	}

	itm->way.next = NULL;
}

/**
 * @brief Updates the ways one can drive from itm
 *
 * This updates the list of possible ways to drive to from itm. The item "itm" is on
 * and the next navigation item are excluded.
 *
 * @param itm The item that should be updated
 * @param graph_map The route graph's map that these items are on 
 */
static void
navigation_itm_ways_update(struct navigation_itm *itm, struct map *graph_map) 
{
	struct map_selection coord_sel;
	struct map_rect *g_rect; /* Contains a map rectangle from the route graph's map */
	struct item *i,*sitem;
	struct attr sitem_attr,direction_attr;
	struct navigation_way *w,*l;

	navigation_itm_ways_clear(itm);

	/* These values cause the code in route.c to get us only the route graph point and connected segments */
	coord_sel.next = NULL;
	coord_sel.u.c_rect.lu = itm->start;
	coord_sel.u.c_rect.rl = itm->start;
	/* the selection's order is ignored */
	
	g_rect = map_rect_new(graph_map, &coord_sel);
	
	i = map_rect_get_item(g_rect);
	if (!i || i->type != type_rg_point) { /* probably offroad? */
		map_rect_destroy(g_rect);
		return ;
	}

	w = NULL;
	
	while (1) {
		i = map_rect_get_item(g_rect);

		if (!i) {
			break;
		}
		
		if (i->type != type_rg_segment) {
			continue;
		}
		
		if (!item_attr_get(i,attr_street_item,&sitem_attr)) {
			dbg(lvl_warning, "Got no street item for route graph item in entering_straight()\n");
			continue;
		}		

		if (!item_attr_get(i,attr_direction,&direction_attr)) {
			continue;
		}

		sitem = sitem_attr.u.item;
		if (sitem->type == type_street_turn_restriction_no || sitem->type == type_street_turn_restriction_only)
			continue;

		if (item_is_equal(itm->way.item,*sitem) || ((itm->prev) && item_is_equal(itm->prev->way.item,*sitem))) {
			continue;
		}

		l = w;
		w = g_new0(struct navigation_way, 1);
		w->dir = direction_attr.u.num;
		w->item = *sitem;
		w->next = l;
		navigation_way_init(w);	/* calculte and set w->angle2 */
	}

	map_rect_destroy(g_rect);
	
	itm->way.next = w;
}

/**
 * @brief Destroys navigation items associated with a navigation object.
 *
 * This function destroys all or some of the {@code navigation_itm} instances associated with
 * {@code this_}, starting with the first one. Data structures associated with the items will
 * also be freed.
 *
 * @param this_ The navigation object whose command instances are to be destroyed
 * @param end The first navigation item to keep. If it is NULL or not found in the list, all items
 * will be destroyed.
 */
static void
navigation_destroy_itms_cmds(struct navigation *this_, struct navigation_itm *end)
{
	struct navigation_itm *itm;
	struct navigation_command *cmd;
	dbg(lvl_info,"enter this_=%p this_->first=%p this_->cmd_first=%p end=%p\n", this_, this_->first, this_->cmd_first, end);
	if (this_->cmd_first)
		dbg(lvl_info,"this_->cmd_first->itm=%p\n", this_->cmd_first->itm);
	while (this_->first && this_->first != end) {
		itm=this_->first;
		dbg(lvl_debug,"destroying %p\n", itm);
		item_hash_remove(this_->hash, &itm->way.item);
		this_->first=itm->next;
		if (this_->first)
			this_->first->prev=NULL;
		if (this_->cmd_first && this_->cmd_first->itm == itm->next) {
			cmd=this_->cmd_first;
			this_->cmd_first=cmd->next;
			if (cmd->next) {
				cmd->next->prev = NULL;
			}
			if (cmd->maneuver)
				g_free(cmd->maneuver);
			g_free(cmd);
		}
	
		map_convert_free(itm->way.name);
		map_convert_free(itm->way.name_systematic);
		map_convert_free(itm->way.exit_ref);
		map_convert_free(itm->way.exit_label);
		free_list(itm->way.destination);
		navigation_itm_ways_clear(itm);
		g_free(itm);
	}
	if (! this_->first)
		this_->last=NULL;
	if (! this_->first && end) 
		dbg(lvl_error,"end wrong\n");
	dbg(lvl_info,"ret this_->first=%p this_->cmd_first=%p\n",this_->first, this_->cmd_first);
}

static void
navigation_itm_update(struct navigation_itm *itm, struct item *ritem)
{
	struct attr length, time, speed;

	if (! item_attr_get(ritem, attr_length, &length)) {
		dbg(lvl_error,"no length\n");
		return;
	}
	if (! item_attr_get(ritem, attr_time, &time)) {
		dbg(lvl_error,"no time\n");
		return;
	}
	if (! item_attr_get(ritem, attr_speed, &speed)) {
		dbg(lvl_error,"no time\n");
		return;
	}

	dbg(lvl_debug,"length=%ld time=%ld speed=%ld\n", length.u.num, time.u.num, speed.u.num);
	itm->length=length.u.num;
	itm->time=time.u.num;
	itm->speed=speed.u.num;
}

/*@ brief creates and adds a new navigation_itm to a linked list of such
 *
 * routeitem has an attr. streetitem, but that is only and id and a map,
 * allowing to fetch the actual streetitem, that will live under the same name.
 *
 *@ param : the navigation
 *@ param : the routeitem from which to create a navigation item
 *@ return : the new navigation_itm (used nowhere)
 *
 */
 
static struct navigation_itm *
navigation_itm_new(struct navigation *this_, struct item *routeitem)
{
	struct navigation_itm *ret=g_new0(struct navigation_itm, 1);
	int i=0;
	struct item *streetitem;
	struct map *graph_map = NULL;
	struct attr street_item,direction,route_attr;
	struct map_rect *mr;
	struct attr attr;
	struct coord c[5];
	struct coord exitcoord;

	if (routeitem) {
		ret->streetname_told=0;
		if (! item_attr_get(routeitem, attr_street_item, &street_item)) {
			dbg(lvl_warning, "no street item\n");
			g_free(ret);
			ret = NULL;
			return ret;
		}

		if (item_attr_get(routeitem, attr_direction, &direction))
			ret->way.dir=direction.u.num;
		else
			ret->way.dir=0;

		streetitem=street_item.u.item;
		ret->way.item=*streetitem;
		item_hash_insert(this_->hash, streetitem, ret);

		mr=map_rect_new(streetitem->map, NULL);  

		struct map *tmap = streetitem->map;

		if (! (streetitem=map_rect_get_item_byid(mr, streetitem->id_hi, streetitem->id_lo))) {
			g_free(ret);
			map_rect_destroy(mr);
			return NULL;
		}

		if (item_attr_get(streetitem, attr_flags, &attr))
			ret->way.flags=attr.u.num;

		if (item_attr_get(streetitem, attr_street_name, &attr))
			ret->way.name=map_convert_string(streetitem->map,attr.u.str);

		if (item_attr_get(streetitem, attr_street_name_systematic, &attr))
			ret->way.name_systematic=map_convert_string(streetitem->map,attr.u.str);

		if (ret->way.flags && (ret->way.flags & AF_ONEWAY))
			{
				if (item_attr_get(streetitem, attr_street_destination, &attr))
				{
					char *destination_raw;
					destination_raw=map_convert_string(streetitem->map,attr.u.str);
					dbg(lvl_debug,"destination_raw =%s\n",destination_raw);
					split_string_to_list(&(ret->way),destination_raw, ';');
					g_free(destination_raw);
				}
			}
		else
			{
				if (ret->way.dir == 1)
				{
					if (item_attr_get(streetitem, attr_street_destination_forward, &attr))
					{
						char *destination_raw;
						destination_raw=map_convert_string(streetitem->map,attr.u.str);
						dbg(lvl_debug,"destination_raw forward =%s\n",destination_raw);
						split_string_to_list(&(ret->way),destination_raw, ';');
						g_free(destination_raw);
					}

				}
				if (ret->way.dir == -1)
				{
					if (item_attr_get(streetitem, attr_street_destination_backward, &attr))
					{
						char *destination_raw;
						destination_raw=map_convert_string(streetitem->map,attr.u.str);
						dbg(lvl_debug,"destination_raw backward =%s\n",destination_raw);
						split_string_to_list(&(ret->way),destination_raw, ';');
						g_free(destination_raw);
					}
				}
			}
		
		navigation_itm_update(ret, routeitem);


		while (item_coord_get(routeitem, &c[i], 1))
		{
			dbg(lvl_debug, "coord %d 0x%x 0x%x\n", i, c[i].x ,c[i].y);
			if (i < 4)
				i++;
			else
			{
				c[2]=c[3];
				c[3]=c[4];
			}
		}

		i--;
		if (i>=1)
		{
			ret->way.angle2=road_angle(&c[0], &c[1], 0);
			ret->angle_end=road_angle(&c[i-1], &c[i], 0);
		}
		ret->start=c[0];
		ret->end=c[i];

		/*	If we have a ramp check the map for higway_exit info,
		 *  but only on the first node of the ramp.
		 *  Ramps with nodes in reverse order and oneway=-1 are not
		 *  specifically handled, but no occurence known so far either.
		 *  If present, obtain exit_ref, exit_label and exit_to
		 *  from the map.
		 *
		 */
		if (streetitem->type == type_ramp )
		{
			struct map_selection mselexit;
			struct item *rampitem;
			dbg(lvl_debug,"test ramp\n");
			mselexit.next = NULL;
			mselexit.u.c_rect.lu = c[0] ;
			mselexit.u.c_rect.rl = c[0] ;
			mselexit.range = item_range_all;
			mselexit.order = 18;

			map_rect_destroy(mr);
			mr = map_rect_new	(tmap, &mselexit);

			while ((rampitem=map_rect_get_item(mr)))
			{
				if (rampitem->type == type_highway_exit && item_coord_get(rampitem, &exitcoord, 1)
							&& exitcoord.x == c[0].x && exitcoord.y == c[0].y)
				{
					while (item_attr_get(rampitem, attr_any, &attr))
					{
						if (attr.type && attr.type == attr_label)
						{
							dbg(lvl_debug,"exit_label=%s\n",attr.u.str);
							ret->way.exit_label= map_convert_string(streetitem->map,attr.u.str);
						}
						if (attr.type == attr_ref)
						{
							dbg(lvl_debug,"exit_ref=%s\n",attr.u.str);
							ret->way.exit_ref= map_convert_string(streetitem->map,attr.u.str);
						}
						if (attr.type == attr_exit_to)
						{
							/* use exit_to as a fall back in case :
							 * - there is no regular destination info
							 * - we are not coming from a ramp already
							 */
							if (attr.u.str && !ret->way.destination && (this_->last) && (!(this_->last->way.item.type == type_ramp)))
							{
								char *destination_raw;
								destination_raw=map_convert_string(streetitem->map,attr.u.str);
								dbg(lvl_debug,"destination_raw from exit_to =%s\n",destination_raw);
								if ((split_string_to_list(&(ret->way),destination_raw, ';')) < 2)
								/*
								 * if a first try did not result in an actual splitting
								 * retry with ',' as a separator because in France a bad
								 * mapping practice exists to separate exit_to with ','
								 * instead of ';'
								 */
								(split_string_to_list(&(ret->way),destination_raw, ','));
								g_free(destination_raw);
							}
						}
					}
				}
			}
		}

		item_attr_get(routeitem, attr_route, &route_attr);
		graph_map = route_get_graph_map(route_attr.u.route);

		dbg(lvl_debug,"i=%d start %d end %d '%s' \n", i, ret->way.angle2, ret->angle_end, ret->way.name_systematic);
		map_rect_destroy(mr);
	} else {
		if (this_->last)
			ret->start=ret->end=this_->last->end;
	}
	if (! this_->first)
		this_->first=ret;
	if (this_->last) {
		this_->last->next=ret;
		ret->prev=this_->last;
		if (graph_map) {
			navigation_itm_ways_update(ret,graph_map);
		}
	}
	dbg(lvl_debug,"ret=%p\n", ret);
	this_->last=ret;
	return ret;
}

/**
 * @brief Counts how many times a driver could turn right/left 
 *
 * This function counts how many times the driver theoretically could
 * turn right/left between two navigation items, not counting the final
 * turn itself.
 *
 * @param from The navigation item which should form the start
 * @param to The navigation item which should form the end
 * @param direction Set to < 0 to count turns to the left >= 0 for turns to the right
 * @return The number of possibilities to turn or -1 on error
 */
static int
count_possible_turns(struct navigation *nav, struct navigation_itm *from, struct navigation_itm *to, int direction)
{
	int count;
	struct navigation_itm *curr;
	struct navigation_way *w;

	count = 0;
	curr = from->next;
	while (curr && (curr != to)) {
		w = curr->way.next;

		while (w) {
			if (is_way_allowed(nav, w, 4)) {
				if (direction < 0) {
					if (angle_delta(curr->prev->angle_end, w->angle2) < 0) {
						count++;
						break;
					}
				} else {
					if (angle_delta(curr->prev->angle_end, w->angle2) > 0) {
						count++;
						break;
					}				
				}
			}
			w = w->next;
		}
		curr = curr->next;
	}

	if (!curr) { /* from does not lead to to? */
		return -1;
	}

	return count;
}

/**
 * @brief Calculates distance and time to the destination
 *
 * This function calculates the distance and the time to the destination of a
 * navigation. If incr is set, this is only calculated for the first navigation
 * item, which is a lot faster than re-calculation the whole destination, but works
 * only if the rest of the navigation already has been calculated.
 *
 * @param this_ The navigation whose destination / time should be calculated
 * @param incr Set this to true to only calculate the first item. See description.
 */
static void
calculate_dest_distance(struct navigation *this_, int incr)
{
	int len=0, time=0, count=0;
	struct navigation_itm *next,*itm=this_->last;
	dbg(lvl_debug, "enter this_=%p, incr=%d\n", this_, incr);
	if (incr) {
		if (itm) {
			dbg(lvl_info, "old values: (%p) time=%d lenght=%d\n", itm, itm->dest_length, itm->dest_time);
		} else {
			dbg(lvl_info, "old values: itm is null\n");
		}
		itm=this_->first;
		next=itm->next;
		dbg(lvl_info, "itm values: time=%d lenght=%d\n", itm->length, itm->time);
		dbg(lvl_info, "next values: (%p) time=%d lenght=%d\n", next, next->dest_length, next->dest_time);
		itm->dest_length=next->dest_length+itm->length;
		itm->dest_count=next->dest_count+1;
		itm->dest_time=next->dest_time+itm->time;
		dbg(lvl_info, "new values: time=%d lenght=%d\n", itm->dest_length, itm->dest_time);
		return;
	}
	while (itm) {
		len+=itm->length;
		time+=itm->time;
		itm->dest_length=len;
		itm->dest_time=time;
		itm->dest_count=count++;
		itm=itm->prev;
	}
	dbg(lvl_debug,"len %d time %d\n", len, time);
}

/**
 * @brief Checks if two navigation items are on the same street
 *
 * This function checks if two navigation items are on the same street. It returns
 * true if either their name or their "systematic name" (e.g. "A6" or "B256") are the
 * same.
 *
 * @param old The first item to be checked
 * @param new The second item to be checked
 * @return True if both old and new are on the same street
 */
static int
is_same_street2(char *old_name, char *old_name_systematic, char *new_name, char *new_name_systematic)
{
	if (old_name && new_name && !strcmp(old_name, new_name)) {
		dbg(lvl_debug,"is_same_street: '%s' '%s' vs '%s' '%s' yes (1.)\n", old_name_systematic, new_name_systematic, old_name, new_name);
		return 1;
	}
	if (old_name_systematic && new_name_systematic && !strcmp(old_name_systematic, new_name_systematic)) {
		dbg(lvl_debug,"is_same_street: '%s' '%s' vs '%s' '%s' yes (2.)\n", old_name_systematic, new_name_systematic, old_name, new_name);
		return 1;
	}
	dbg(lvl_debug,"is_same_street: '%s' '%s' vs '%s' '%s' no\n", old_name_systematic, new_name_systematic, old_name, new_name);
	return 0;
}


static int maneuver_category(enum item_type type)
{
	switch (type) {
	case type_street_0:
		return 1;
	case type_street_1_city:
		return 2;
	case type_street_2_city:
		return 3;
	case type_street_3_city:
		return 4;
	case type_street_4_city:
		return 5;
	case type_highway_city:
		return 7;
	case type_street_1_land:
		return 2;
	case type_street_2_land:
		return 3;
	case type_street_3_land:
		return 4;
	case type_street_4_land:
		return 5;
	case type_street_n_lanes:
		return 6;
	case type_highway_land:
		return 7;
	case type_ramp:
		return 0;
	case type_roundabout:
		return 0;
	case type_ferry:
		return 0;
	default:
		return 0;
	}


}

/**
 * @brief Checks whether a way is allowed
 *
 * This function checks whether a given vehicle is permitted to enter a given way by comparing the
 * access and one-way restrictions of the way against the settings in {@code nav->vehicleprofile}.
 * Turn restrictions are not taken into account.
 *
 * @return True if entry is permitted, false otherwise. If {@code nav->vehicleprofile} is null, true is returned.
 */

 /* (jandegr) this gets called from within show_maneuver with mode=3 for roundabouts
 * and with mode=4 from within count_possible_turns() for the use with
 * 'take the manieTH road to the left/right'
 * However over here mode is ignored, so the 'manieTH' road excludes unallowed oneway's,
 * but IMHO it should count all drivable roads. For roundabouts it seems to be ok.
 *
 */

static int
is_way_allowed(struct navigation *nav, struct navigation_way *way, int mode)
{
	if (!nav->vehicleprofile)
		return 1;
	return !way->flags || ((way->flags & (way->dir >= 0 ? nav->vehicleprofile->flags_forward_mask : nav->vehicleprofile->flags_reverse_mask)) == nav->vehicleprofile->flags);
}

/**
 * @brief Checks whether a way has motorway-like characteristics
 *
 * Motorway-like means one of the following:
 *
 * item type is highway_land or highway_city (OSM: highway=motorway)
 * item type is street_n_lanes (OSM: highway=trunk) and way is one-way
 * {@code extended} is true and item type is either ramp or street_service
 *
 * @param way The way to examine
 * @param extended Whether to consider ramps and service roads to be motorway-like
 * @return True for motorway-like, false otherwise
 */
static int
is_motorway_like(struct navigation_way *way, int extended)
{
	if ((way->item.type == type_highway_land) || (way->item.type == type_highway_city) || ((way->item.type == type_street_n_lanes) && (way->flags & AF_ONEWAYMASK)))
		return 1;
	if ((extended) && ((way->item.type == type_ramp) || (way->item.type == type_street_service)))
		return 1;
	return 0;
}

/**
 * @brief Checks whether a way is a ramp
 *
 * @param way The way to be examined
 * @return True for ramp, false otherwise
 */
static int
is_ramp(struct navigation_way *way) {
	if (way->item.type == type_ramp)
		return 1;
	return 0;
}

/**
 * @brief Checks if navit has to create a maneuver to drive from old to new
 *
 * This function checks if it has to create a "maneuver" - i.e. guide the user - to drive
 * from "old" to "new".
 *
 * @param old The old navigation item, where we're coming from
 * @param new The new navigation item, where we're going to
 * @param maneuver Pointer to a buffer that will receive a pointer to a {@code struct navigation_maneuver}
 * in which detailed information on the maneuver will be stored. The buffer may receive a null pointer
 * for some cases that do not require a maneuver. If a non-null pointer is returned, the caller is responsible
 * for freeing up the buffer once it is no longer needed.
 * @return True if navit should guide the user, false otherwise
 */
static int
maneuver_required2 (struct navigation *nav, struct navigation_itm *old, struct navigation_itm *new, struct navigation_maneuver **maneuver)
{
	struct navigation_maneuver m; /* if the function returns true, this will be passed in the maneuver argument */
	struct navigation_itm *ni; /* temporary navigation item used for comparisons that examine previous or subsequent maneuvers */
	int ret=0;
	int dw; /* temporary bearing difference between old and w (way being examined) */
	int dlim; /* if no other ways are within +/- dlim, the maneuver is unambiguous */
	int dc; /* if new and another way are within +/-min_turn_limit and on the same side, bearing difference for the other way; else d */
	char *r=NULL; /* human-legible reason for announcing or not announcing the maneuver */
	struct navigation_way *w; /* temporary way to examine */
	int wcat;
	int junction_limit = 100; /* maximum distance between two carriageways at a junction */
	int motorways_left = 0, motorways_right = 0; /* number of motorway-like roads left or right of new->way */
	int route_leaves_motorway = 0; /* when the maneuver changes from a motorway-like road to a ramp,
	                                * whether a subsequent maneuver leaves the motorway (changing direction
	                                * is considered leaving the motorway) */

	*maneuver = NULL;

	m.type = type_nav_none;
	m.delta = angle_delta(old->angle_end, new->way.angle2);
	m.merge_or_exit = mex_none;
	m.is_complex_t_junction = 0;
	m.num_options = 0;
	m.num_new_motorways = 0;
	m.num_other_ways = 0;
	m.num_similar_ways = 0;
	m.old_cat = maneuver_category(old->way.item.type);
	m.new_cat = maneuver_category(new->way.item.type);
	m.max_cat = -1;
	m.left = -180;
	m.right = 180;
	m.is_unambiguous = 1;
	/* Check whether the street keeps its name */
	m.is_same_street = is_same_street2(old->way.name, old->way.name_systematic, new->way.name, new->way.name_systematic);

	dbg(lvl_debug,"enter %p %p %p\n",old, new, maneuver);
/*	dbg(0,"old=%s %s, new=%s %s, angle old=%d, angle new=%d, d=%i\n ",old->way.name,old->way.name_systematic,new->way.name,new->way.name_systematic,old->angle_end, new->way.angle2,d); */
	if (!new->way.next || (new->way.next && (new->way.next->angle2 == new->way.angle2) && !new->way.next->next)) {
		/* No announcement necessary (with extra magic to eliminate duplicate ways) */
		r="no: Only one possibility";
	} else if (!new->way.next->next && new->way.next->item.type == type_ramp && !is_way_allowed(nav,new->way.next,1)) {
		/* If the other way is only a ramp and it is one-way in the wrong direction, no announcement necessary */
		r="no: Only ramp and unallowed direction ";
		ret=0;
	}
	if (! r) {
		/* Announce exit from roundabout, but not entry or staying in it */
		if ((old->way.flags & AF_ROUNDABOUT) && ! (new->way.flags & AF_ROUNDABOUT)) {
			r="yes: leaving roundabout";
			ret=1;
		} else 	if (!new->way.next->next && !(old->way.flags & AF_ROUNDABOUT) && (new->way.flags & AF_ROUNDABOUT) && (new->way.next->flags & AF_ROUNDABOUT)) {
			/* this rather complicated construct makes sure we suppress announcements
			 * only when we're entering a roundabout AND there are no other options */
			r="no: entering roundabout";
		} else if ((old->way.flags & AF_ROUNDABOUT) && (new->way.flags & AF_ROUNDABOUT)) {
			r="no: staying in roundabout";
		}
	}
	if (!r) {
		/* Analyze all options (including new->way).
		 * Anything that iterates over the whole set of options should be done here. This avoids
		 * looping over the entire set of ways multiple times, which aims to improve performance
		 * and predictability (because the same filter is applied to the ways being analyzed).
		 */
		w = &(new->way);
		int through_segments = 0;
		dc=m.delta;
		/* Check whether the street keeps its name */
		while (w) {
			/* in case of overlapping ways, avoid counting the way on the route twice */
			if ((w->angle2 != new->way.angle2) || (w == &(new->way))) {
				dw=angle_delta(old->angle_end, w->angle2);
				if (is_way_allowed(nav,w,1)) {
					m.num_options++;
					/* ways of similar category */
					if (maneuver_category(w->item.type) == m.old_cat) {
						/* TODO: decide if a maneuver_category difference of 1 is still similar */
						m.num_similar_ways++;
					}
					/* motorway-like ways */
					if (is_motorway_like(w, 0)) {
						m.num_new_motorways++;
					} else if (!is_motorway_like(w, 1)) {
						m.num_other_ways++;
					}
					if (w != &(new->way)) {
						/* if we're exiting from a motorway, check which side of the ramp the motorway is on */
						if (is_motorway_like(w, 0) && is_motorway_like(&(old->way), 0) && new->way.item.type == type_ramp) {
							if (dw < m.delta)
								motorways_left++;
							else
								motorways_right++;
						}

						if (dw < m.delta) {
							if (dw > m.left)
								m.left=dw;
						} else {
							if (dw < m.right)
								m.right=dw;
						}

						/* If almost-straight ways are present, the maneuver is ambiguous. We are counting only ways having
						 * a nonzero maneuver category (street_0 or higher), excluding ramps, service roads and anything closed
						 * to motorized traffic. Exceptions apply when the new way itself has a maneuver category of 0.
						 * Note that this is in addition for the dlim calculations we do further below, as they fail to
						 * catch some ambiguous cases for very low deltas. */
						if ((dw > -min_turn_limit) && (dw < min_turn_limit) && ((maneuver_category(w->item.type) != 0) || (maneuver_category(new->way.item.type) == 0)))
							m.is_unambiguous = 0;

						if (dw < 0) {
							if (dw > -min_turn_limit && m.delta < 0 && m.delta > -min_turn_limit)
								dc=dw;
						} else {
							if (dw < min_turn_limit && m.delta > 0 && m.delta < min_turn_limit)
								dc=dw;
						}
						wcat=maneuver_category(w->item.type);
						/* If any other street has the same name, we can't use the same name criterion.
						 * Exceptions apply if we're coming from a motorway-like road and:
						 * - the other road is motorway-like (a motorway might split up temporarily) or
						 * - the other road is a ramp or service road (they are sometimes tagged with the name of the motorway)
						 * The second one is really a workaround for bad tagging practice in OSM. Since entering
						 * a ramp always creates a maneuver, we don't expect the workaround to have any unwanted
						 * side effects.
						 */
						if (m.is_same_street && is_same_street2(old->way.name, old->way.name_systematic, w->name, w->name_systematic) && (!is_motorway_like(&(old->way), 0) || (!is_motorway_like(w, 0) && w->item.type != type_ramp)) && is_way_allowed(nav,w,2))
							//if (m.is_same_street && is_same_street2(old->way.name, old->way.name_systematic, w->name, w->name_systematic) && (!is_motorway_like(&(old->way), 0) || !is_motorway_like(w, 1)) && is_way_allowed(nav,w,2))
							m.is_same_street=0;
						/* If the route category changes to a lower one but another road has the same route category as old,
						 * it is not clear which of the two the driver would perceive as the "same street", hence reset is_same_street */
						/* Mark if the street has a higher or the same category */
						if (wcat > m.max_cat)
							m.max_cat=wcat;
					} /* if w != new->way */
					/* if is_way_allowed */
				} else {
					/* If we're merging onto a motorway, check which side of the ramp the motorway is on.
					 * This requires examining the candidate ways which are NOT allowed. */
					if (is_motorway_like(w, 0) && is_motorway_like(&(new->way), 0) && old->way.item.type == type_ramp) {
						if (dw < 0)
							motorways_left++;
						else
							motorways_right++;
					}
					/* if !is_way_allowed */
				} /* if is_way_allowed || !is_way_allowed */
				if ((w->flags & AF_ONEWAYMASK) && is_same_street2(new->way.name, new->way.name_systematic, w->name, w->name_systematic))
					/* count through_segments (even if they are not allowed) to check if we are at a complex T junction */
					through_segments++;
			} /* if w... */
			w = w->next;
		} /* while w */
		if (m.num_options <= 1) {
			if ((abs(m.delta) >= min_turn_limit) && (through_segments == 2)) {
				/* FIXME: maybe there are cases with more than 2 through_segments...? */
				/* If we have to make a considerable turn (min_turn_limit or more),
				 * check whether we are approaching a complex T junction from the "stem"
				 * (which would need an announcement).
				 * Complex means that the through road is a dual-carriageway road.
				 * To find this out, we need to analyze the previous maneuvers.
				 */
				int hist_through_segments = 0;
				int hist_dist = old->length; /* distance between previous and current maneuver */
				ni = old;
				while (ni && (hist_through_segments == 0) && (hist_dist <= junction_limit)) {
					struct navigation_way *w = ni->way.next;
					while (w) {
						if ((w->flags & AF_ONEWAYMASK) && (is_same_street2(new->way.name, new->way.name_systematic, w->name, w->name_systematic)))
							hist_through_segments++;
						w = w->next;
					}
					ni = ni->prev;
					if (ni)
						hist_dist += ni->length;
				}
				if (hist_through_segments == 2) {
					/* FIXME: see above for number of through_segments */
					ret=1;
					m.is_complex_t_junction = 1;
					r="yes: turning into dual-carriageway through-road of T junction";
				}
			}
		}
	}
	if (!r && abs(m.delta) > 75) {
		/* always make an announcement if you have to make a sharp turn */
		r="yes: delta over 75";
		ret=1;
	} else if (!r && abs(m.delta) >= min_turn_limit) {
		if ((m.new_cat >= maneuver_category(type_street_2_city)) && (m.num_similar_ways > 1)) {
			/* When coming from street_2_* or higher category road, check if
			 * - we have multiple options of the same category and
			 * - we have to make a considerable turn (at least min_turn_limit)
			 * If both is the case, ANNOUNCE.
			 */
			ret=1;
			r="yes: more than one similar road and delta >= min_turn_limit";
		}
	}
	if ((!r) && (m.num_options <= 1))
		r="no: only one option permitted";
	if (!r) {
		if (is_motorway_like(&(old->way), 0) && (m.num_other_ways == 0) && (m.num_new_motorways > 1)) {
			/* If we are at a motorway interchange, ANNOUNCE
			 * We are assuming a motorway interchange when old way and at least
			 * two possible ways are motorway-like and allowed.
			 * If any of the possible ways is neither motorway-like nor a ramp,
			 * we are probably on a trunk road with level crossings and not
			 * at a motorway interchange.
			 */
			r="yes: motorway interchange (multiple motorways)";
			m.merge_or_exit = mex_interchange;
			ret=1;
		} else if (is_motorway_like(&(old->way), 0) && (m.num_other_ways == 0) && (!m.is_same_street)) {
			/* Another sign that we are at a motorway interchange is if the street name changes
			 */
			r="yes: motorway interchange (name changes)";
			/* TODO: tell motorway interchanges from exits */
			/* m.merge_or_exit = mex_interchange; */
			ret=1;
		} else if ((new->way.item.type == type_ramp) && ((m.num_other_ways == 0) || (abs(m.delta) >= min_turn_limit)) && ((m.left > -90) || (m.right < 90))) {
			/* Motorway ramps can be confusing, therefore we need to lower the bar for announcing a maneuver.
			 * When the new way is a ramp, we check for the following criteria:
			 * - All available ways are either motorway-like or ramps.
			 *   This prevents this rule from firing in non-motorway settings, which is needed to avoid
			 *   superfluous maneuvers when the minor road of a complex T junction is a ramp.
			 * - If the above is not met, the maneuver must involve a turn (min_turn_limit or more) to enter the ramp.
			 * - Additionally, there must be one way (other than the new way) within +/-90°.
			 *   This prevents the rule from essentially announcing "don't do the U turn" where the ramps for
			 *   two opposite directions merge.
			 * If the criteria are satisfied, announce.
			 */
			r="yes: entering ramp";
			ret=1;
		}
	}
	if (!r) {
		/* get the delta limit for checking for other streets. It is lower if the street has no other
		   streets of the same or higher category */
		if (m.new_cat < m.old_cat)
			dlim=80;
		else
			dlim=120;
		/* if the street is really straight, the others might be closer to straight */
		if (abs(m.delta) < 20)
			dlim/=2;
		/* if both old and new way have a category of 0, or if both ways and at least one other way are
		 * in the same category and no other ways are higher,
		 * dlim is 620/256 (roughly 2.5) times the delta of the maneuver */
		if ((m.max_cat == m.new_cat && m.max_cat == m.old_cat) || (m.new_cat == 0 && m.old_cat == 0))
			dlim=abs(m.delta)*620/256;
		/* if both old, new and highest other category differ by no more than 1,
		 * dlim is just higher than the delta (so another way with a delta of exactly -d will be treated as ambiguous) */
		else if (max(max(m.old_cat, m.new_cat), m.max_cat) - min(min(m.old_cat, m.new_cat), m.max_cat) <= 1)
			dlim = abs(m.delta) + 1;
		/* if both old and new way are in higher than highest encountered category,
		 * dlim is 128/256 times (i.e. one half) the delta of the maneuver */
		else if (m.max_cat < m.new_cat && m.max_cat < m.old_cat)
			dlim=abs(m.delta)*128/256;
		/* if no other ways are within +/-dlim, the maneuver is unambiguous */
		if (m.left >= -dlim || m.right <= dlim)
			m.is_unambiguous = 0;
		/* if another way is within +/-min_turn_limit and on the same side as new, the maneuver is ambiguous */
		if (dc != m.delta) {
			dbg(1,"m.delta %d vs dc %d\n",m.delta,dc);
			m.is_unambiguous=0;
		}
		if (!m.is_same_street && m.is_unambiguous < 1) { /* FIXME: why < 1? */
			ret=1;
			r="yes: different street and ambiguous";
		} else
			r="no: same street or unambiguous";
#ifdef DEBUG
		r=g_strdup_printf("%s: d %d left %d right %d dlim=%d cat old:%d new:%d max:%d unambiguous=%d same_street=%d", ret==1?"yes":"no", m.delta, m.left, m.right, dlim, m.old_cat, m.new_cat, m.max_cat, m.is_unambiguous, m.is_same_street);
#endif
	}

	if (m.merge_or_exit == mex_none) {
		if (old->way.item.type == type_ramp && is_motorway_like(&(new->way), 0)) {
			if (motorways_left)
				m.merge_or_exit = mex_merge_left;
			else if (motorways_right)
				m.merge_or_exit = mex_merge_right;
			/* if there are no motorways on either side, we are not merging
			 * (more likely the start of a motorway) */

			if (m.merge_or_exit != mex_none) {
				ret=1;
				if (!r)
					r = "yes: merging onto motorway-like road";
			}
		} else if (new->way.item.type == type_ramp && is_motorway_like(&(old->way), 0)) {
			/* Detect interchanges - if:
			 * - we're entering a ramp,
			 * - the route is taking us onto another motorway-like road and
			 * - none of the maneuvers in between connects to any non-motorway roads,
			 * set m.merge_or_exit = mex_interchange.
			 * The last check is to prevent direction changes (i.e. exit motorway and take access ramp
			 * for opposite direction) from being misinterpreted as interchanges. */
			ni = new->next;
			while (!route_leaves_motorway && ni && (ni->way.item.type == type_ramp)) {
				w = &(ni->way);
				while (!route_leaves_motorway && w) {
					route_leaves_motorway = !is_motorway_like(w, 1);
					w = w->next;
				}
				ni = ni->next;
			}
			if (ni && !route_leaves_motorway && is_motorway_like(&(ni->way), 0))
				m.merge_or_exit = mex_interchange;
			else
				if (motorways_left && (m.left > -90))
					m.merge_or_exit = mex_exit_right;
				else if (motorways_right && (m.right < 90))
					m.merge_or_exit = mex_exit_left;
				/* if there are no motorways within +/-90 degrees on either side, this is not an exit
				 * (more likely the end of a motorway) */

			if (m.merge_or_exit != mex_none) {
				ret=1;
				if (!r)
					r = "yes: exiting motorway-like road";
			}
		}
	}

	if (ret) {
		*maneuver = g_new(struct navigation_maneuver, 1);
		memcpy(*maneuver, &m, sizeof(struct navigation_maneuver));
	}
	if (r)
		dbg(lvl_debug, "%s %s %s -> %s %s %s: %s, delta=%i, merge_or_exit=%i\n", item_to_name(old->way.item.type), old->way.name_systematic, old->way.name, item_to_name(new->way.item.type), new->way.name_systematic, new->way.name, r, m.delta, m.merge_or_exit);
	return ret;
}

/**
 * @brief Creates a new {@code struct navigation_command} for a maneuver.
 *
 * This function also parses {@code maneuver} and sets its {@code type} appropriately so that other
 * functions can rely on that. This is currently underway and only a few maneuver types are implemented
 * thus far.
 *
 * @param this_ The navigation object
 * @param itm The navigation item following the maneuver
 * @param maneuver The {@code struct navigation_maneuver} returned by {@code maneuver_required2()}. For the destination,
 * initialize a zeroed-out {@code struct navigation_maneuver} and set its {@code type} member to {@code type_nav_destination}
 * prior to calling this function.
 */
static struct navigation_command *
command_new(struct navigation *this_, struct navigation_itm *itm, struct navigation_maneuver *maneuver)
{
	struct navigation_command *ret=g_new0(struct navigation_command, 1);
	enum item_type r = type_none, l = type_none;

	/* Some variables needed only for roundabouts */
	int len=0; /* length of roundabout segment */
	int roundabout_length; /* estimated total length of roundabout */
	int angle=0;
	int entry_angle; /* angle at which we enter the roundabout, offset by 90 degrees */
	int exit_angle; /* angle at which we leave the roundabout, offset by 90 degrees */
	int entry_road_angle, exit_road_angle; /* angles before and after approach segments */
	struct navigation_itm *itm2; /* items before itm to examine, up to first roundabout segment on route */
	struct navigation_itm *itm3; /* items before itm2 and after itm to examine */
	struct navigation_way *w; /* continuation of the roundabout after we leave it */
	struct navigation_way *w2; /* segment of the roundabout leading to the point at which we enter it */
	int dtsir = 0; /* delta to stay in roundabout */
	int d, dmax = 0; /* when examining deltas of roundabout approaches, current and maximum encountered */
	int delta1, delta2, error1 = 0, error2; /* for roundabout delta calculated with different approaches, and error margin */
	int delta3; /* roundabout delta calculated from entry_road_angle and exit_road_angle, currently not used in calculations */
	int dist_left; /* when examining ways around the roundabout to a certain threshold, the distance we have left to go */
	int central_angle; /* approximate central angle for the roundabout arc that is part of the route */

	dbg(lvl_debug,"enter this_=%p itm=%p maneuver=%p delta=%d\n", this_, itm, maneuver, maneuver->delta);
	ret->maneuver = maneuver;
	ret->delta=maneuver->delta;
	ret->itm=itm;

	/* Possible maneuver types:
	 * nav_none                    (default, change wherever we encounter it – unless the maneuver is a merge, which has only merge_or_exit)
	 * nav_straight                (set below)
	 * nav_keep_{left|right}       (set below)
	 * nav_{right|left}_{1..3}     (set below)
	 * nav_turnaround              (TODO: when we have a U turn without known direction? Needs full implementation!)
	 * nav_turnaround_{left|right} (set below)
	 * nav_roundabout_{r|l}{1..8}  (set below, special handling)
	 * nav_exit_{left|right}       (do not set here)
	 * nav_merge_{left|right}      (do not set here)
	 * nav_destination             (if this is set, leave it)
	 * nav_position                (do not set here)
	 */

	if (ret->maneuver->type != type_nav_destination) {
		// TODO: make this a separate function (improves code legibility)
		/* if we're leaving a roundabout, special handling is needed:
		 * - calculate effective bearing change (between entry and exit),
		 * - set length,
		 * - set ret->maneuver->type to nav_roundabout_{r|l}{1..8}.
		 *
		 * Bearing change must be as close as possible to how drivers would perceive it.
		 * Builds prior to r2017 used the difference between the last way before and the first way after the roundabout,
		 * which tends to overestimate the angle when V-shaped approach roads are involved.
		 *
		 * In r2017 a different approach was introduced, which essentially distorts the roads so they enter and leave
		 * the roundabout at a 90 degree angle. (To make calculations simpler, tangents of the roundabout at the entry
		 * and exit points are used, with one of them reversed in direction, instead of the approach roads.)
		 * However, this approach tends to underestimate the angle when the distance between approach roads is large.
		 *
		 * Project HighFive combines these two approaches, using a weighted average between the two so that the errors
		 * cancel each other out as far as possible.
		 */
		/* FIXME: this will not catch cases in which entry and exit share the same node and we just *touch* the roundabout */
		if (itm && itm->prev && !(itm->way.flags & AF_ROUNDABOUT) && (itm->prev->way.flags & AF_ROUNDABOUT)) {
			/* Find continuation of roundabout after the exit. Don't simply use itm->way.next here, it will break
			 * if a node in the roundabout is shared by more than one way */
			w = itm->way.next;
			while (w && !(w->flags & AF_ROUNDABOUT))
				w = w->next;
			if (w) {
				/* When exiting a roundabout, w should never be null, thus this
				 * code will always be executed. Checking for the condition anyway ensures
				 * that botched map data (roundabout ending with nowhere else to go) will not
				 * cause a crash. For the same reason we're using dtsir with a default value of 0.
				 */

				/* approximate error for delta2: central angle (=bearing change) of roundabout segment after exit */
				error2 = abs(angle_delta(itm->prev->angle_end, navigation_way_get_exit_angle(w)));

				dtsir = angle_delta(itm->prev->angle_end, w->angle2);
				dbg(lvl_debug,"delta to stay in roundabout %d\n", dtsir);

				exit_angle=angle_median(itm->prev->angle_end, w->angle2);
				dbg(lvl_debug,"exit %d median from %d,%d\n", exit_angle,itm->prev->angle_end, w->angle2);

				/* Move back to where we enter the roundabout, calculate length in roundabout */
				itm2=itm;
				while (itm2->prev && (itm2->prev->way.flags & AF_ROUNDABOUT)) {
					itm2=itm2->prev;
					len+=itm2->length;
					angle=itm2->angle_end;
				}

				/* Find the segment of the roundabout leading up to the point at which we enter it. Again, don't simply
				 * use itm2->way.next here, it will break if a node in the roundabout is shared by more than one way */
				w2 = itm2->way.next;
				while (w2 && !(w2->flags & AF_ROUNDABOUT))
					w2 = w2->next;

				/* Calculate entry angle */
				if (itm2 && w2) {
					/* improve error estimate for delta2: average of central angles (=bearing change) of the roundabout
					 * segments before entry and after exit */
					error2 = (error2 + abs(angle_delta(angle_opposite(itm2->way.angle2), navigation_way_get_exit_angle(w2)))) / 2;
					entry_angle=angle_median(angle_opposite(itm2->way.angle2), w2->angle2);
					dbg(lvl_debug,"entry %d median from %d(%d),%d\n", entry_angle,angle_opposite(itm2->way.angle2), itm2->way.angle2, itm2->way.next->angle2);
				} else {
					entry_angle=angle_opposite(angle);
				} /* endif itm2 && w2 */
				dbg(lvl_debug,"entry %d exit %d\n", entry_angle, exit_angle);

				delta2 = angle_delta(entry_angle, exit_angle);
				dbg(lvl_debug,"delta2 %d error %d\n", delta2, error2);

				if (itm2->prev) {
					delta1 = angle_delta(itm2->prev->angle_end, itm->way.angle2);
					/* If we are turning around and there are V-shaped approach segments, delta1 will point
					 * in the wrong direction. This may also happen with sharp turns, taking the last exit.
					 * Hence we need to add or subtract 360 degrees in these cases.
					 * This is the case when both delta1 and delta2 are somewhat close to +/-180
					 * but in opposite directions. We're using 0 degrees as the threshold, which should be OK because
					 * delta2 tends to underestimate the central angle. (+/-90 fails to catch some cases.)*/
					if ((ret->delta > dtsir) && (delta2 < 0) && (delta1 > 90)) {
						/* counterclockwise roundabout */
						dbg(lvl_debug,"correcting delta1 %d to %d\n", delta1, delta1 - 360);
						delta1 -= 360;
					} if ((ret->delta < dtsir) && (delta2 > 0) && (delta1 < -90)) {
						/* clockwise roundabout */
						dbg(lvl_debug,"correcting delta1 %d to %d\n", delta1, delta1 + 360);
						delta1 += 360;
					}

					/* Now try to figure out the error range for delta1. Errors are caused by turns in the approach segments
					 * just before the roundabout. We use the last segment before the approach as a reference.
					 * We assume the approach to begin when one of the following is true:
					 * - a way turns into a ramp
					 * - a way turns into a one-way road
					 * - a certain distance from the roundabout, proportional to its circumference, is exceeded
					 * Simply comparing bearings at these points may cause confusion with certain road layouts (namely
					 * S-shaped dual-carriageway roads), hence we examine the entire approach segment and take the largest
					 * delta (relative to the end of the approach segment) which we encounter.
					 * This is done for both ends of the roundabout.
					 */

					/* Approximate roundabout circumference based on len and approximate central angle of route segment.
					 * The central angle is approximated using the unweighted average of delta1 and delta2,
					 * which is somewhat crude but should be OK for error estimates. */
					central_angle = abs((delta1 + delta2) / 2 + ((ret->delta < dtsir) ? 180 : -180));
					roundabout_length = len * 360 / central_angle;
					dbg(lvl_debug,"roundabout_length = %dm (for central_angle = %d degrees)\n", roundabout_length, central_angle);

					/* in the case of separate carriageways, approach roads become hard to identify, thus we keep a cap on distance.
					 * Currently this is at most half the length of the roundabout. */
					/* FIXME: experiment with different values here */
					dist_left = roundabout_length / 2;
					dbg(lvl_debug,"examining roads for up to %dm to estimate error for delta1\n", dist_left);

					/* examine items before roundabout */
					itm3 = itm2->prev; /* last segment before roundabout */
					while (itm3->prev && (dist_left >= itm3->length)) {
						if ((itm3->next && is_ramp(&(itm3->next->way)) && !is_ramp(&(itm3->way))) || !(itm3->way.flags & AF_ONEWAYMASK)) {
							dist_left = 0; /* to make sure we don't examine the following way in depth */
							break;
						}
						d = navigation_way_get_max_delta(&(itm3->way), map_projection(this_->map), itm2->prev->angle_end, itm3->length - dist_left, -1);
						if ((d != invalid_angle) && (abs(d) > abs(dmax)))
							dmax = d;
						dist_left -= itm3->length;
						itm3 = itm3->prev;
						if (itm3->next && itm3->next->way.next) {
							dist_left = 0;
							break;
						}
					}
					if (dist_left == 0) {
						d = angle_delta(itm3->angle_end, itm2->prev->angle_end);
					} else if (dist_left <= itm3->length) {
						d = navigation_way_get_max_delta(&(itm3->way), map_projection(this_->map), itm2->prev->angle_end, itm3->length - dist_left, -1);
					} else {
						/* not enough objects in navigation map, use most distant one */
						d = angle_delta(itm3->way.angle2, itm2->prev->angle_end);
					}
					if ((d != invalid_angle) && (abs(d) > abs(dmax)))
						dmax = d;
					error1 = abs(dmax);
					entry_road_angle = itm2->prev->angle_end - dmax;
					dbg(lvl_debug,"entry_road_angle %d (%d - %d)\n", entry_road_angle, itm2->prev->angle_end, dmax);

					/* examine items after roundabout */
					dmax = 0;
					dist_left = roundabout_length / 2;
					itm3 = itm;
					while (itm3->next && (dist_left >= itm3->length)) {
						if ((itm3->prev && is_ramp(&(itm3->prev->way)) && !is_ramp(&(itm3->way))) || !(itm3->way.flags & AF_ONEWAYMASK)) {
							dist_left = 0; /* to make sure we don't examine the following way in depth */
							break;
						}
						d = navigation_way_get_max_delta(&(itm3->way), map_projection(this_->map), itm->way.angle2, dist_left, 1);
						if ((d != invalid_angle) && (abs(d) > abs(dmax)))
							dmax = d;
						dist_left -= itm3->length;
						itm3 = itm3->next;
						if (itm3->way.next) {
							dist_left = 0;
							break;
						}
					}
					if (dist_left == 0) {
						d = angle_delta(itm->way.angle2, itm3->way.angle2);
					} else if (dist_left <= itm3->length) {
						d = navigation_way_get_max_delta(&(itm3->way), map_projection(this_->map), itm->way.angle2, dist_left, 1);
					} else {
						/* not enough objects in navigation map, use most distant one */
						d = angle_delta(itm->way.angle2, itm3->angle_end);
					}
					if ((d != invalid_angle) && (abs(d) > abs(dmax)))
						dmax = d;
					error1 = (error1 + abs(dmax) + 1) / 2;
					exit_road_angle = itm->way.angle2 + dmax;
					dbg(lvl_debug,"exit_road_angle %d (%d + %d)\n", exit_road_angle, itm->way.angle2, dmax);

					dbg(lvl_debug,"delta1 %d error %d\n", delta1, error1);

					/* We now have two approximations delta1 and delta2 with corresponding errors.
					 * However, deltas are biased as each constitutes a boundary of its possible range.
					 * We need to correct this so that each delta will be in the middle of its range.
					 * This requires knowing the direction of the roundabout.
					 * To avoid mis-guessing, we use two approaches and use results only if both agree.
					 * Note that we divide the error range by two even if we can't guess the direction.
					 * While not 100% correct, it has no impact on results as long as the ratio is maintained.
					 * Adding 1 before dividing ensures we round up. */
					error1 = (error1 + 1) / 2;
					error2 = (error2 + 1) / 2;
					if ((ret->delta > dtsir) && (delta1 < delta2)) {
						/* counterclockwise; exit right; delta1 (approach ways) further left (i.e. smaller) than delta2 (tangents) */
						delta1 += error1;
						delta2 -= error2;
						dbg(lvl_debug,"Corrected delta1 %d error %d, delta2 %d error %d\n", delta1, error1, delta2, error2);
					} else if ((ret->delta < dtsir) && (delta1 > delta2)) {
						/* clockwise; exit left; delta1 (approach ways) further right (greater) than delta2 (tangents) */
						delta1 -= error1;
						delta2 += error2;
						dbg(lvl_debug,"Corrected delta1 %d error %d, delta2 %d error %d\n", delta1, error1, delta2, error2);
					}

					delta3 = angle_delta(entry_road_angle, exit_road_angle);
					dbg(lvl_debug,"delta3 %d\n", delta3);

					if ((error1 == 0) && (error2 == 0))
						ret->roundabout_delta = (delta1 + delta2) / 2;
					else
						ret->roundabout_delta = (delta1 * error2 + delta2 * error1) / (error1 + error2);
					dbg(lvl_debug,"roundabout_delta %d\n", ret->roundabout_delta);
				} else {
					/* we don't know where we entered the roundabout, so we can't calculate delta1 */
					ret->roundabout_delta = delta2;
				} /* endif itm2->prev */
				ret->length=len+roundabout_extra_length;
			} /* if w */

			/* set ret->maneuver->type */
			switch (((180 + 22) - ret->roundabout_delta) / 45) {
			case 0:
			case 1:
				r = type_nav_roundabout_r1;
				l = type_nav_roundabout_l7;
				break;
			case 2:
				r = type_nav_roundabout_r2;
				l = type_nav_roundabout_l6;
				break;
			case 3:
				r = type_nav_roundabout_r3;
				l = type_nav_roundabout_l5;
				break;
			case 4:
				r = type_nav_roundabout_r4;
				l = type_nav_roundabout_l4;
				break;
			case 5:
				r = type_nav_roundabout_r5;
				l = type_nav_roundabout_l3;
				break;
			case 6:
				r = type_nav_roundabout_r6;
				l = type_nav_roundabout_l2;
				break;
			case 7:
				r = type_nav_roundabout_r7;
				l = type_nav_roundabout_l1;
				break;
			case 8:
				r = type_nav_roundabout_r8;
				l = type_nav_roundabout_l8;
				break;
			}
			dbg(lvl_debug,"delta %d\n", ret->delta);
			/* if delta to leave the roundabout (ret->delta) is less than delta to stay in roundabout (dtsir),
			 * we're exiting to the left, so we're probably in a clockwise roundabout, and vice versa */
			if (ret->delta < dtsir)
				ret->maneuver->type = l;
			else
				ret->maneuver->type = r;

			/* if leaving roundabout */
		} else {
			/* set ret->maneuver->type */
			if (ret->delta >= min_turn_limit) {
				/* if the route turns right:
				 * examine delta to determine strength of turn */
				if (ret->delta < angle_straight )
					ret->maneuver->type = type_nav_straight;
				else if (ret->delta < turn_2_limit)
					ret->maneuver->type = type_nav_right_1;
				else if (ret->delta < sharp_turn_limit)
					ret->maneuver->type = type_nav_right_2;
				else if (ret->delta < u_turn_limit)
					ret->maneuver->type = type_nav_right_3;
				else
					/* TODO: refine turnaround detection, fall back to type_nav_right_3 */
					ret->maneuver->type=type_nav_turnaround_right;
			} else if (ret->delta <= -min_turn_limit) {
				/* if the route turns left:
				 * examine delta to determine strength of turn */
				if (-ret->delta < turn_2_limit)
					ret->maneuver->type = type_nav_left_1;
				else if (-ret->delta < sharp_turn_limit)
					ret->maneuver->type = type_nav_left_2;
				else if (-ret->delta < u_turn_limit)
					ret->maneuver->type = type_nav_left_3;
				else
					/* TODO: refine turnaround detection, fall back to type_nav_left_3 */
					ret->maneuver->type=type_nav_turnaround_left;
			} else {
				/* if the route goes straight:
				 * If there's another way on one side of the route within 2 * min_turn_limit (not both - the expression below is a logical XOR),
				 * the maneuver is "keep left" or "keep right", else it is "go straight".
				 * Note that neighbors are not necessarily straight.
				 * The boundary may need some tweaking, (2 * min_turn_limit) may not be ideal but it's a first shot which ensures that other straight ways
				 * will always fulfill the neighbor criteria. */
				int has_left_neighbor = (ret->maneuver->left - ret->delta > 2 * -min_turn_limit);
				int has_right_neighbor = (ret->maneuver->right - ret->delta < 2 * min_turn_limit);
				if (!(has_left_neighbor) != !(has_right_neighbor)) {
					if (has_left_neighbor)
						ret->maneuver->type = type_nav_keep_right;
					else
						ret->maneuver->type = type_nav_keep_left;
				} else
					ret->maneuver->type = type_nav_straight;
			} /* endif ret->delta */
		}
	}
/*temporary solution to recover some motorway
 *exits that get a (slight)turn left/rigth 
 */
		if (itm->way.exit_ref)
		{
			if (ret->delta < 0){
				ret->maneuver->merge_or_exit = mex_exit_left;
			}
			if (ret->delta > 0){
				ret->maneuver->merge_or_exit = mex_exit_right;
			}
			if (ret->delta < angle_straight )
				ret->maneuver->type = type_nav_straight;
		}

	if (this_->cmd_last) {
		this_->cmd_last->next=ret;
		ret->prev = this_->cmd_last;
	}
	this_->cmd_last=ret;

	if (!this_->cmd_first)
		this_->cmd_first=ret;
	return ret;
}

static void
make_maneuvers(struct navigation *this_, struct route *route)
{
	struct navigation_itm *itm, *last=NULL, *last_itm=NULL;
	struct navigation_maneuver *maneuver;
	itm=this_->first;
	this_->cmd_last=NULL;
	this_->cmd_first=NULL;
	while (itm) {
		if (last) {
			if (maneuver_required2(this_, last_itm, itm, &maneuver)) {
				command_new(this_, itm, maneuver);
			}
		} else
			last=itm;
		last_itm=itm;
		itm=itm->next;
	}
	maneuver = g_new0(struct navigation_maneuver, 1);
	maneuver->type = type_nav_destination;
	command_new(this_, last_itm, maneuver);
}

static int
contains_suffix(char *name, char *suffix)
{
	if (!suffix)
		return 0;
	if (strlen(name) < strlen(suffix))
		return 0;
	return !navit_utf8_strcasecmp(name+strlen(name)-strlen(suffix), suffix);
}



static char *
replace_suffix(char *name, char *search, char *replace)
{
	int len=strlen(name)-strlen(search);
	char *ret=g_malloc(len+strlen(replace)+1);
	strncpy(ret, name, len);
	strcpy(ret+len, replace);
	if (isupper(name[len])) {
		ret[len]=toupper(ret[len]);
	}

	return ret;
}



static char *
navigation_item_destination(struct navigation *nav, struct navigation_command *cmd, struct navigation_itm *next, char *prefix)
{
	char *ret=NULL,*name1,*sep,*name2;
	char *name=NULL,*name_systematic=NULL;
	int i,sex;
	int vocabulary1=1;
	int vocabulary2=1;
	struct attr attr;
	struct navigation_itm *itm = cmd->itm;

	if (! prefix)
		prefix="";
        /* check the configuration of navit.xml */
	if (nav->speech && speech_get_attr(nav->speech, attr_vocabulary_name, &attr, NULL))
		vocabulary1=attr.u.num; /* shall the street name be announced? */
	if (nav->speech && speech_get_attr(nav->speech, attr_vocabulary_name_systematic, &attr, NULL))
		vocabulary2=attr.u.num; /* shall the systematic name be announced? */


	/* On motorway links don't announce the name of the ramp as this is done by name_systematic and the street_destination. */
	if (vocabulary1 && (itm->way.item.type != type_ramp))
		name=itm->way.name;

	if (vocabulary2)
		name_systematic=itm->way.name_systematic;


if (cmd->maneuver && cmd->maneuver->type && ((cmd->maneuver->merge_or_exit==mex_merge_left)
			||(cmd->maneuver->merge_or_exit==mex_merge_right) ))
	{
		if (name || name_systematic)
		/* TRANSLATORS: %1$s is the name_systematic of the next road to merge onto, %2$s it's name*/
		return g_strdup_printf(_("onto the %1$s %2$s"),name_systematic ? name_systematic : "",
				name ? name : "");
		else return g_strdup("");

	}


	if(!name && !name_systematic && itm->way.item.type == type_ramp && vocabulary2) {
			 
		if(next->way.item.type == type_ramp)
			return NULL;
		else
			return g_strdup_printf("%s%s",prefix,_("into the ramp"));

	}


	if (!name && !name_systematic)
		return NULL;
	if (name) {
		sex=unknown;
		name1=NULL;
		for (i = 0 ; i < sizeof(suffixes)/sizeof(suffixes[0]) ; i++) {


			if (contains_suffix(name,suffixes[i].fullname)) {
				sex=suffixes[i].sex;
				name1=g_strdup(name);
				break;
			}
			if (contains_suffix(name,suffixes[i].abbrev)) {
				sex=suffixes[i].sex;
				name1=replace_suffix(name, suffixes[i].abbrev, suffixes[i].fullname);
				break;
			}
		}
		if (name_systematic) {
			name2=name_systematic;
			sep=" ";
		} else {
			name2="";
			sep="";
		}
		switch (sex) {
		case unknown:
			/* TRANSLATORS: Arguments: 1: Prefix (Space if required) 2: Street Name 3: Separator (Space if required), 4: Systematic Street Name */
			ret=g_strdup_printf(_("%sinto the street %s%s%s"),prefix,name, sep, name2);
			break;
		case male:
			/* TRANSLATORS: Arguments: 1: Prefix (Space if required) 2: Street Name 3: Separator (Space if required), 4: Systematic Street Name. Male form. The stuff after | doesn't have to be included */
			ret=g_strdup_printf(_("%sinto the %s%s%s|male form"),prefix,name1, sep, name2);
			break;
		case female:
			/* TRANSLATORS: Arguments: 1: Prefix (Space if required) 2: Street Name 3: Separator (Space if required), 4: Systematic Street Name. Female form. The stuff after | doesn't have to be included */
			ret=g_strdup_printf(_("%sinto the %s%s%s|female form"),prefix,name1, sep, name2);
			break;
		case neutral:
			/* TRANSLATORS: Arguments: 1: Prefix (Space if required) 2: Street Name 3: Separator (Space if required), 4: Systematic Street Name. Neutral form. The stuff after | doesn't have to be included */
			ret=g_strdup_printf(_("%sinto the %s%s%s|neutral form"),prefix,name1, sep, name2);
			break;
		}
		g_free(name1);
			
	} else
		/* TRANSLATORS: gives the name of the next road to turn into (into the E17) */
		ret=g_strdup_printf(_("%sinto the %s"),prefix,name_systematic);
	name1=ret;
	while (name1 && *name1) {
		switch (*name1) {
		case '|':
			*name1='\0';
			break;
		case '/':
			*name1++=' ';
			break;
		default:
			name1++;
			break;
		}
	}
	return ret;
}

/* @brief creates turn by turn guidance sentences for the speech and for the route description
 *
 */
static char *
show_maneuver(struct navigation *nav, struct navigation_itm *itm, struct navigation_command *cmd, enum attr_type type, int connect)
{

	int distance=itm->dest_length-cmd->itm->dest_length;
	char *d=NULL,*ret=NULL;
	char *street_destination_announce=NULL;
	int level;
	int skip_roads = 0;
	int count_roundabout;
	struct navigation_itm *cur;
	int tellstreetname = 0;
	char *destination = NULL;
	char *street_destination = NULL;
	char * instruction = NULL;

	if (connect)
		level = -2; /* level = -2 means "connect to another maneuver via 'then ...'" */
	else
		level = 1;


	if (type != attr_navigation_long_exact)
		distance=round_distance(distance);
	if (type == attr_navigation_speech)
	{
		if (nav->turn_around && nav->turn_around == nav->turn_around_limit)
		{
			navigation_set_turnaround(nav, nav->turn_around_count+1);
			return g_strdup(_("When possible, please turn around"));
		}
		navigation_set_turnaround(nav, 0);
		if (!connect)
			level=navigation_get_announce_level_cmd(nav, itm, cmd, distance-cmd->length);
		dbg(lvl_debug,"distance=%d level=%d type=0x%x\n", distance, level, itm->way.item.type);
	}


	street_destination=select_announced_destinations(cmd);
	if (street_destination)
		/* TRANSLATORS: the argument is the destination to follow */
		street_destination_announce=g_strdup_printf(_("towards %s"),street_destination);
	else street_destination_announce=g_strdup("");
	g_free(street_destination);

	if (cmd->itm->prev->way.flags & AF_ROUNDABOUT)
	{
		cur = cmd->itm->prev;
		count_roundabout = 0;
		while (cur && (cur->way.flags & AF_ROUNDABOUT))
		{
			if (cur->next->way.next && is_way_allowed(nav,cur->next->way.next,3))
			/* If the next segment has no exit or the exit isn't allowed, don't count it */
				count_roundabout++;
			cur = cur->prev;
		}
		switch (level)
		{
			case 3:
				d=get_distance(nav, distance, type, 1);
				return g_strdup_printf(_("Follow the road for the next %s"), d);
			case 2:
				return g_strdup(_("Enter the roundabout soon"));
			case 1:
				d = get_distance(nav, distance, type, 0);
				/* TRANSLATORS: %s is the distance to the roundabout */
				ret = g_strdup_printf(_("Enter the roundabout %s"), d);
				g_free(d);
				return ret;
			case -2:
				/* TRANSLATORS: first arg. is the manieth exit, second arg. is the destination to follow */
				return g_strdup_printf(_("then leave the roundabout at the %1$s %2$s"), get_exit_count_str(count_roundabout),street_destination_announce);
			case 0:
				/* TRANSLATORS: first arg. is the manieth exit, second arg. is the destination to follow */
				return g_strdup_printf(_("Leave the roundabout at the %1$s %2$s"), get_exit_count_str(count_roundabout),street_destination_announce);
		}
	}

	if (cmd->maneuver && cmd->maneuver->type)
	{
		if (cmd->itm->next)
		{
			if(type == attr_navigation_speech)
			{ /* In voice mode */
			/* In Voice Mode only tell the street name in level 1 or in level 0 if level 1	was skipped */
				if (level == 1)
				{ /* we are close to the intersection */
					cmd->itm->streetname_told = 1; // remeber to be checked when we turn
					tellstreetname = 1; // Ok so we tell the name of the street
				}
				if (level == 0)
				{
					if(cmd->itm->streetname_told == 0) /* we are right at the intersection */
						tellstreetname = 1;
					else
						cmd->itm->streetname_told = 0;  /* reset just in case we come to the same street again */
				}

			}
			else
				tellstreetname = 1;
		}


		switch (level)
		{
			case 2 :
				d=g_strdup(_("soon"));
				break;
			case 1 :
				d=get_distance(nav, distance, attr_navigation_short, 0);
				break;
			case 0 :
				d=g_strdup(_("now"));
				break;
			case -2 :
				d=g_strdup(_("then"));
				break;
			default :
				d = g_strdup("");
				break;
		}

		if (!(cmd->maneuver->merge_or_exit == mex_none ))
		{
			char *folded_exit_label=NULL;
			char *folded_street_destination_announce=NULL;
			switch (cmd->maneuver->merge_or_exit)
			{
				case mex_merge_left:
					if (tellstreetname)
						destination=navigation_item_destination(nav, cmd, itm, NULL);
					else destination = g_strdup("");
					g_free(instruction);
					/* TRANSLATORS: the first arg. is distance, the second is the phrase 'onto ...'  */
					instruction = g_strdup_printf(_("%1$s merge left %2$s"),d,destination);
					break;
				case mex_merge_right:
					if (tellstreetname)
						destination=navigation_item_destination(nav, cmd, itm, NULL);
					else destination = g_strdup("");
					g_free(instruction);
					/* TRANSLATORS: the first arg. is distance, the second is the phrase 'onto ...'  */
					instruction = g_strdup_printf(_("%1$s merge right %2$s"),d,destination);
					break;
					/* for mex_exit_left/right, exit_label is not announced in case it is
					* a substring of destination info to avoid redundancy and not let the sentence
					* become too long
					*/
				case mex_exit_left:
					g_free(instruction);
					if (cmd->itm->way.exit_label)
						folded_exit_label = linguistics_casefold(cmd->itm->way.exit_label);
					else folded_exit_label = g_strdup("");
					folded_street_destination_announce = linguistics_casefold(street_destination_announce);
					/* TRANSLATORS: the first arg. is distance, the second is exit_ref and the third is exit_label */
					instruction = g_strdup_printf(_("%1$s left exit %2$s %3$s"),d,cmd->itm->way.exit_ref ? cmd->itm->way.exit_ref : "",
							(!strstr(folded_street_destination_announce,folded_exit_label)) ? cmd->itm->way.exit_label ? cmd->itm->way.exit_label :"" :"");
					g_free(folded_exit_label);
					g_free(folded_street_destination_announce);
					break;
				case mex_exit_right:
					g_free(instruction);
					if (cmd->itm->way.exit_label)
						folded_exit_label = linguistics_casefold(cmd->itm->way.exit_label);
					else folded_exit_label = g_strdup("");
					folded_street_destination_announce = linguistics_casefold(street_destination_announce);
					/* TRANSLATORS: the first arg. is distance, the second is exit_ref and the third is exit_label */
					instruction = g_strdup_printf(_("%1$s right exit %2$s %3$s"),d,cmd->itm->way.exit_ref ? cmd->itm->way.exit_ref : "",
										(!strstr(folded_street_destination_announce,folded_exit_label)) ?
												cmd->itm->way.exit_label ? cmd->itm->way.exit_label :"" :"");
					g_free(folded_exit_label);
					g_free(folded_street_destination_announce);
					break;
			}

			if (!instruction)
			{
				char *at;
				if (cmd->itm->way.exit_ref || cmd->itm->way.exit_label)
					at = g_strdup_printf("%1$s%2$s %3$s",cmd->itm->way.exit_ref ? (_( " at the exit ")) : (_(" at the interchange ")),
										cmd->itm->way.exit_ref ? cmd->itm->way.exit_ref : "",cmd->itm->way.exit_label ? cmd->itm->way.exit_label : " ");
				else at = g_strdup(" ");
				switch (cmd->maneuver->type)
				{
					case type_nav_straight :
						/* TRANSLATORS: the first arg. is distance, the second is where to do the maneuvre */
						instruction = g_strdup_printf(_("%1$s continue straight%2$s"),d, at);
						break;
					case type_nav_keep_right :
						/* TRANSLATORS: the first arg. is distance, the second is where to do the maneuvre */
						instruction = g_strdup_printf(_("%1$s keep right%2$s"),d, at);
						break;
					case type_nav_keep_left :
						/* TRANSLATORS: the first arg. is distance, the second is where to do the maneuvre */
						instruction = g_strdup_printf(_("%1$s keep left%2$s"),d, at);
						break;
					default :
						/* in case we end up here in the merge_or_exit situation, it can be either
						* a classic turn instruction or an unsuitable instruction for
						* motorways. For street_n_lanes this criterion could be relaxed.
						*/
						dbg(lvl_error,"unhandled instruction %s\n",attr_to_name(cmd->maneuver->type));
						break;
				}
				g_free(at);
			}
		}
	}
	if (!instruction)
	{
		switch (cmd->maneuver->type)
		{
			case type_nav_straight :
				/* TRANSLATORS: the arg. is distance  */
				instruction = g_strdup_printf(_("%1$s continue straight"),d);
				break;
			case type_nav_keep_right :
				/* TRANSLATORS: the arg. is distance  */
				instruction = g_strdup_printf(_("%1$s keep right"),d);
				break;
			case type_nav_keep_left :
				/* TRANSLATORS: the arg. is distance  */
				instruction = g_strdup_printf(_("%1$s keep left"),d);
				break;
			case type_nav_right_1 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level == 0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,90);
				if (skip_roads)
				{
					if (skip_roads < 6)
						instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("right")));
						/*and preserve skip_roads to signal we already have an instruction*/
					else
					{
						g_free(d);
						d=g_strdup_printf(_("after %i roads"),skip_roads);
						skip_roads = 0; /*signal an instruction still has to be created*/
					}
				}
				if (!skip_roads)
					/* TRANSLATORS: the first arg. is strength, the second is direction, the third is distance, the fourth is destination  */
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(_("easily ")),(_("right")),d,destination);
				break;
			case type_nav_right_2 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level ==0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,90);
				if (skip_roads)
				{
					if (skip_roads < 6)
						instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("right")));
					else
						{
							g_free(d);
							d=g_strdup_printf(_("after %i roads"),skip_roads);
							skip_roads = 0;
						}
				}
				if (!skip_roads)
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(""),(_("right")),d,destination);
				break;
			case type_nav_right_3 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level == 0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,90);
				if (skip_roads)
				{
					if (skip_roads < 6)
						instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("right")));
					else
					{
						g_free(d);
						d=g_strdup_printf(_("after %i roads"),skip_roads);
						skip_roads = 0;
					}
				}
				if (!skip_roads)
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(_("strongly ")),(_("right")),d,destination);
				break;
			case type_nav_left_1 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level == 0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,-90);
				if (skip_roads)
					{
						if (skip_roads < 6)
							instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("left")));
						else
						{
							g_free(d);
							d=g_strdup_printf(_("after %i roads"),skip_roads);
							skip_roads = 0;
						}
					}
				if (!skip_roads)
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(_("easily ")),(_("left")),d,destination);
				break;
			case type_nav_left_2 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level == 0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,-90);
				if (skip_roads)
					{
						if (skip_roads < 6)
							instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("left")));
						else
						{
							g_free(d);
							d=g_strdup_printf(_("after %i roads"),skip_roads);
							skip_roads = 0;
						}
					}
				if (!skip_roads)
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(""),(_("left")),d,destination);
				break;
			case type_nav_left_3 :
				if (tellstreetname)
					destination=navigation_item_destination(nav, cmd, itm, NULL);
				if (!destination)
					destination = g_strdup("");
				if (level==-2 || level == 0)
					skip_roads = count_possible_turns(nav,cmd->prev ? cmd->prev->itm : nav->first,cmd->itm,-90);
				if (skip_roads)
				{
					if (skip_roads < 6)
						instruction = g_strdup_printf(_("Take the %1$s road to the %2$s"),get_count_str(skip_roads+1),(_("left")));
					else
					{
						g_free(d);
						d=g_strdup_printf(_("after %i roads"),skip_roads);
						skip_roads = 0;
					}
				}
				if (!skip_roads)
					instruction = g_strdup_printf(_("Turn %1$s%2$s %3$s %4$s"),(_("strongly ")),(_("left")),d,destination);
				break;
			case  type_nav_turnaround_left:
				/* TRANSLATORS: the arg. is distance  */
				instruction = g_strdup_printf(_("%1$s left turnaround"),d);
				break;
			case  type_nav_turnaround_right:
				/* TRANSLATORS: the arg. is distance  */
				instruction = g_strdup_printf(_("%1$s right turnaround"),d);
				break;
			case  type_nav_none:
				/*An empty placeholder that we can use in the future for
				 * some motorway commands that are now suppressed but we
				 * can in some cases make it say here :
				 * 'follow destination blabla' without any further driving instructions,
				 * in cases where relevant destination info is available.
				 * Even if there is no driving command to be announced, in some cases
				 * there is an overhead roadsign in preparation of an upcoming road-split,
				 * and then we can give usefull info to the driver.
				 *
				 *  UNTESTED !
				 *
				 */
				instruction = g_strdup("follow ");
				break;
			case type_nav_destination:
				/* the old code used to clear the route destination when this was the only
				 * instruction left. Was that usefull ?
				 * Should be tested with the old code what happens if the driver
				 * 'overshoots' the destination and the route destination is already cleared.
				 * I suppose it will now keep guiding the user to destination untill another one
				 * is set or a 'stop navigation' action is done using the gui.
				 */
				if (level == -2)
					instruction=g_strdup(_("then you have reached your destination."));
				else
					/* TRANSLATORS: the arg. is distance  */
					instruction=g_strdup_printf(_("You have reached your destination %s"), d);
				break;
			default:
				dbg(lvl_error,"unhandled instruction %s\n",attr_to_name(cmd->maneuver->type));
				break;
		}
	}
	switch (level)
	{
		case 3:
			d=get_distance(nav, distance, type, 1);
			ret=g_strdup_printf(_("Follow the road for the next %s"), d);
			break;
		case 2:
			ret= g_strdup_printf(("%1$s %2$s"),instruction,street_destination_announce);
			break;
		case 1:
			ret= g_strdup_printf(("%1$s %2$s"),instruction,street_destination_announce);
			break;
		case -2:
			ret= g_strdup_printf(("%1$s %2$s"),instruction,street_destination_announce);
			break;
		case 0:
			ret= g_strdup_printf(("%1$s %2$s"),instruction,street_destination_announce);
			break;
		default :
			ret= g_strdup_printf(("%1$s %2$s"),instruction,street_destination_announce);
			dbg(lvl_error,"unevaluated speech level\n");
			break;
	}

	g_free(d);
	g_free(destination);
	g_free(instruction);
	g_free(street_destination_announce);
	return ret;

}


/**
 * @brief Creates announcements for maneuvers, plus maneuvers immediately following the next maneuver
 *
 * This function does create an announcement for the current maneuver and for maneuvers
 * immediately following that maneuver, if these are too close and we're in speech navigation.
 *
 * @return An announcement that should be made
 */
static char *
show_next_maneuvers(struct navigation *nav, struct navigation_itm *itm, struct navigation_command *cmd, enum attr_type type)
{
	struct navigation_command *cur,*prev;
	int distance=itm->dest_length-cmd->itm->dest_length;
	int level, i, time;
	int speech_time,time2nav;
	char *ret,*old,*buf,*next;

	if (type != attr_navigation_speech) {
		return show_maneuver(nav, itm, cmd, type, 0); /* We accumulate maneuvers only in speech navigation */
	}

	level=navigation_get_announce_level(nav, itm->way.item.type, distance-cmd->length);

	if (level > 1) {
		return show_maneuver(nav, itm, cmd, type, 0); /* We accumulate maneuvers only if they are close */
	}

	if (cmd->itm->told) {
		return g_strdup("");
	}

	ret = show_maneuver(nav, itm, cmd, type, 0);
	time2nav = navigation_time(itm,cmd->itm->prev);
	old = NULL;

	cur = cmd->next;
	prev = cmd;
	i = 0;
	while (cur && cur->itm) {
		/* We don't merge more than 3 announcements... */
		if (i > 1) { /* if you change this, please also change the value below, that is used to terminate the loop */
			break;
		}
		
		next = show_maneuver(nav,prev->itm, cur, type, 0);
		if (nav->speech)
			speech_time = speech_estimate_duration(nav->speech,next);
		else
			speech_time = -1;
		g_free(next);

		if (speech_time == -1) { /* user didn't set cps */
			speech_time = 30; /* assume 3 seconds */
		}

		time = navigation_time(prev->itm,cur->itm->prev);

		if (time >= (speech_time + 30)) { /* 3 seconds for understanding what has been said */
			break;
		}

		old = ret;
		buf = show_maneuver(nav, prev->itm, cur, type, 1);
		ret = g_strdup_printf("%s, %s", old, buf);
		g_free(buf);
		if (nav->speech && speech_estimate_duration(nav->speech,ret) > time2nav) {
			g_free(ret);
			ret = old;
			i = 2; /* This will terminate the loop */
		} else {
			g_free(old);
		}

		/* If the two maneuvers are *really* close, we shouldn't tell the second one again, because TTS won't be fast enough */
		if (time <= speech_time) {
			cur->itm->told = 1;
		}

		prev = cur;
		cur = cur->next;
		i++;
	}

	return ret;
}

static void
navigation_call_callbacks(struct navigation *this_, int force_speech)
{
	int distance, level = 0;
	void *p=this_;
	if (!this_->cmd_first)
		return;
	callback_list_call(this_->callback, 1, &p);
	dbg(lvl_debug,"force_speech=%d turn_around=%d turn_around_limit=%d\n", force_speech, this_->turn_around, this_->turn_around_limit);
	distance=round_distance(this_->first->dest_length-this_->cmd_first->itm->dest_length);
	if (this_->turn_around_limit && this_->turn_around == this_->turn_around_limit) {
		dbg(lvl_debug,"distance=%d distance_turn=%d\n", distance, this_->distance_turn);
		while (distance > this_->distance_turn) {
			this_->level_last=4;
			level=4;
			force_speech=2;
			if (this_->distance_turn >= 500)
				this_->distance_turn*=2;
			else
				this_->distance_turn=500;
		}
	} else if (!this_->turn_around_limit || this_->turn_around == -this_->turn_around_limit+1) {
		this_->distance_turn=50;
		distance-=this_->cmd_first->length;
		level=navigation_get_announce_level_cmd(this_, this_->first, this_->cmd_first, distance);
		if (level < this_->level_last) {
			/* only tell if the level is valid for more than 3 seconds */
			int speed_distance=this_->first->speed*30/36;
			if (distance < speed_distance || navigation_get_announce_level_cmd(this_, this_->first, this_->cmd_first, distance-speed_distance) == level) {
				dbg(lvl_debug,"distance %d speed_distance %d\n",distance,speed_distance);
				dbg(lvl_debug,"level %d < %d\n", level, this_->level_last);
				this_->level_last=level;
				force_speech=3;
			}
		}
		if (!item_is_equal(this_->cmd_first->itm->way.item, this_->item_last)) {
			this_->item_last=this_->cmd_first->itm->way.item;
			if (this_->delay)
				this_->curr_delay=this_->delay;
			else
				force_speech=5;
		} else {
			if (this_->curr_delay) {
				this_->curr_delay--;
				if (!this_->curr_delay)
					force_speech=4;
			}
		}
	}
	if (force_speech) {
		this_->level_last=level;
		this_->curr_delay=0;
		dbg(lvl_debug,"force_speech=%d distance=%d level=%d type=0x%x\n", force_speech, distance, level, this_->first->way.item.type);
		callback_list_call(this_->callback_speech, 1, &p);
	}
}

static void
navigation_update(struct navigation *this_, struct route *route, struct attr *attr)
{
	struct map *map;
	struct map_rect *mr;
	struct item *ritem;			/* Holds an item from the route map */
	struct item *sitem;			/* Holds the corresponding item from the actual map */
	struct attr street_item,street_direction;
	struct navigation_itm *itm;
	struct attr vehicleprofile;
	int mode=0, incr=0, first=1;
	if (attr->type != attr_route_status)
		return;

	dbg(lvl_debug,"enter %d\n", mode);
	if (attr->u.num == route_status_no_destination || attr->u.num == route_status_not_found || attr->u.num == route_status_path_done_new) 
		navigation_flush(this_);
	if (attr->u.num != route_status_path_done_new && attr->u.num != route_status_path_done_incremental)
		return;
		
	if (! this_->route)
		return;
	map=route_get_map(this_->route);
	if (! map)
		return;
	mr=map_rect_new(map, NULL);
	if (! mr)
		return;
	if (route_get_attr(route, attr_vehicleprofile, &vehicleprofile, NULL))
		this_->vehicleprofile=vehicleprofile.u.vehicleprofile;
	else
		this_->vehicleprofile=NULL;
	dbg(lvl_debug,"enter\n");
	while ((ritem=map_rect_get_item(mr))) {
		if (ritem->type == type_route_start && this_->turn_around > -this_->turn_around_limit+1)
			this_->turn_around--;
		if (ritem->type == type_route_start_reverse && this_->turn_around < this_->turn_around_limit)
			this_->turn_around++;
		if (ritem->type != type_street_route)
			continue;
		if (first && item_attr_get(ritem, attr_street_item, &street_item)) {
			first=0;
			if (!item_attr_get(ritem, attr_direction, &street_direction))
				street_direction.u.num=0;
			sitem=street_item.u.item;
			dbg(lvl_debug,"sitem=%p\n", sitem);
			itm=item_hash_lookup(this_->hash, sitem);
			dbg(lvl_info,"itm for item with id (0x%x,0x%x) is %p\n", sitem->id_hi, sitem->id_lo, itm);
			if (itm && itm->way.dir != street_direction.u.num) {
				dbg(lvl_info,"wrong direction\n");
				itm=NULL;
			}
			navigation_destroy_itms_cmds(this_, itm);
			if (itm) {
				navigation_itm_update(itm, ritem);
				break;
			}
			dbg(lvl_debug,"not on track\n");
		}
		navigation_itm_new(this_, ritem);
	}
	dbg(lvl_info,"turn_around=%d\n", this_->turn_around);
	if (first) 
		navigation_destroy_itms_cmds(this_, NULL);
	else {
		if (! ritem) {
			navigation_itm_new(this_, NULL);
			make_maneuvers(this_,this_->route);
		}
		calculate_dest_distance(this_, incr);
		profile(0,"end");
		navigation_call_callbacks(this_, FALSE);
	}
	map_rect_destroy(mr);
}

static void
navigation_flush(struct navigation *this_)
{
	navigation_destroy_itms_cmds(this_, NULL);
}


void
navigation_destroy(struct navigation *this_)
{
	navigation_flush(this_);
	item_hash_destroy(this_->hash);
	callback_list_destroy(this_->callback);
	callback_list_destroy(this_->callback_speech);
	g_free(this_);
}

int
navigation_register_callback(struct navigation *this_, enum attr_type type, struct callback *cb)
{
	if (type == attr_navigation_speech)
		callback_list_add(this_->callback_speech, cb);
	else
		callback_list_add(this_->callback, cb);
	return 1;
}

void
navigation_unregister_callback(struct navigation *this_, enum attr_type type, struct callback *cb)
{
	if (type == attr_navigation_speech)
		callback_list_remove(this_->callback_speech, cb);
	else
		callback_list_remove(this_->callback, cb);
}

struct map *
navigation_get_map(struct navigation *this_)
{
	struct attr *attrs[5];
	struct attr type,navigation,data,description;
	type.type=attr_type;
	type.u.str="navigation";
	navigation.type=attr_navigation;
	navigation.u.navigation=this_;
	data.type=attr_data;
	data.u.str="";
	description.type=attr_description;
	description.u.str="Navigation";
	
	attrs[0]=&type;
	attrs[1]=&navigation;
	attrs[2]=&data;
	attrs[3]=&description;
	attrs[4]=NULL;
	if (! this_->map)
		this_->map=map_new(NULL, attrs);
        return this_->map;
}

struct map_priv {
	struct navigation *navigation;
};

struct map_rect_priv {
	struct navigation *nav;
	struct navigation_command *cmd;
	struct navigation_command *cmd_next;
	struct navigation_itm *itm;
	struct navigation_itm *itm_next;
	struct navigation_itm *cmd_itm;
	struct navigation_itm *cmd_itm_next;
	struct item item;
	enum attr_type attr_next;
	int ccount;
	int debug_idx;
	struct navigation_way *ways;
	int show_all;
	char *str;
};

static int
navigation_map_item_coord_get(void *priv_data, struct coord *c, int count)
{
	struct map_rect_priv *this=priv_data;
	if (this->ccount || ! count)
		return 0;
	*c=this->itm->start;
	this->ccount=1;
	return 1;
}

static void
navigation_map_item_coord_rewind(void *priv_data)
{
	struct map_rect_priv *this=priv_data;
	this->ccount=0;
}


static int
navigation_map_item_attr_get(void *priv_data, enum attr_type attr_type, struct attr *attr)
{
	struct map_rect_priv *this_=priv_data;
	struct navigation_command *cmd=this_->cmd;
	struct navigation_itm *itm=this_->itm;
	struct navigation_itm *prev=itm->prev;
	attr->type=attr_type;

	if (this_->str) {
		g_free(this_->str);
		this_->str=NULL;
	}

	if (cmd) {
		if (cmd->itm != itm)
			cmd=NULL;	
	}
	switch(attr_type) {
	case attr_level:
		if (cmd) {
			int distance=this_->cmd_itm->dest_length-cmd->itm->dest_length;
			distance=round_distance(distance);
			attr->u.num=navigation_get_announce_level(this_->nav, this_->cmd_itm->way.item.type, distance-cmd->length);
			return 1;
		}
		return 0;
	case attr_navigation_short:
		this_->attr_next=attr_navigation_long;
		if (cmd) {
			this_->str=attr->u.str=show_next_maneuvers(this_->nav, this_->cmd_itm, cmd, attr_type);
			return 1;
		}
		return 0;
	case attr_navigation_long:
		this_->attr_next=attr_navigation_long_exact;
		if (cmd) {
			this_->str=attr->u.str=show_next_maneuvers(this_->nav, this_->cmd_itm, cmd, attr_type);
			return 1;
		}
		return 0;
	case attr_navigation_long_exact:
		this_->attr_next=attr_navigation_speech;
		if (cmd) {
			this_->str=attr->u.str=show_next_maneuvers(this_->nav, this_->cmd_itm, cmd, attr_type);
			return 1;
		}
		return 0;
	case attr_navigation_speech:
		this_->attr_next=attr_length;
		if (cmd) {
			this_->str=attr->u.str=show_next_maneuvers(this_->nav, this_->cmd_itm, this_->cmd, attr_type);
			return 1;
		}
		return 0;
	case attr_length:
		this_->attr_next=attr_time;
		if (cmd) {
			attr->u.num=this_->cmd_itm->dest_length-cmd->itm->dest_length;
			return 1;
		}
		return 0;
	case attr_time:
		this_->attr_next=attr_destination_length;
		if (cmd) {
			attr->u.num=this_->cmd_itm->dest_time-cmd->itm->dest_time;
			return 1;
		}
		return 0;
	case attr_destination_length:
		attr->u.num=itm->dest_length;
		this_->attr_next=attr_destination_time;
		return 1;
	case attr_destination_time:
		attr->u.num=itm->dest_time;
		this_->attr_next=attr_street_name;
		return 1;
	case attr_street_name:
		attr->u.str=itm->way.name;
		this_->attr_next=attr_street_name_systematic;
		if (attr->u.str){
			return 1;}
		return 0;
	case attr_street_name_systematic:
		attr->u.str=itm->way.name_systematic;
		this_->attr_next=attr_street_destination; 
		if (attr->u.str){
			return 1;}
		return 0;
	case attr_street_destination:
		this_->attr_next=attr_name;
		if (itm->way.destination && itm->way.destination->destination)
		this_->str=attr->u.str=select_announced_destinations(cmd);
		else attr->u.str=NULL;
		if (attr->u.str){
			return 1;}
		return 0;

		/* attr_name returns exit_ref and exit_label if available
		 * preceeded by the word 'exit'
		 *
		 * if exit_label alone is available, it returns the word
		 * 'interchange' followed by exit_label
		 *
		 * otherwise returns street name and name_systematic if available
		 *
		 * FIXME should a new attr. be defined for this and if yes, which ?
		 *
		 */
	case attr_name:
		this_->attr_next=attr_debug;
		attr->u.str=NULL;
		if (itm->way.exit_ref)
			this_->str=attr->u.str=g_strdup_printf(("%s %s %s"),_("exit"),itm->way.exit_ref,
					itm->way.exit_label ? itm->way.exit_label :"");
		if (!attr->u.str && itm->way.exit_label)
			this_->str=attr->u.str=g_strdup_printf(("%s %s"),_("interchange"),itm->way.exit_label);
		else if (!attr->u.str && (itm->way.name || itm->way.name_systematic))
			this_->str=attr->u.str=g_strdup_printf(_("%s %s"),
					itm->way.name ? itm->way.name : "",itm->way.name_systematic ? itm->way.name_systematic : "");
		if (attr->u.str){
			return 1;}
		return 0;
	case attr_debug:
		switch(this_->debug_idx) {
		case 0:
			this_->debug_idx++;
			this_->str=attr->u.str=g_strdup_printf("angle:%d (- %d)", itm->way.angle2, itm->angle_end);
			return 1;
		case 1:
			this_->debug_idx++;
			this_->str=attr->u.str=g_strdup_printf("item type:%s", item_to_name(itm->way.item.type));
			return 1;
		case 2:
			this_->debug_idx++;
			if (cmd) {
				this_->str=attr->u.str=g_strdup_printf("delta:%d", cmd->delta);
				return 1;
			}
		case 3:
			this_->debug_idx++;
			if (prev) {
				this_->str=attr->u.str=g_strdup_printf("prev street_name:%s", prev->way.name);
				return 1;
			}
		case 4:
			this_->debug_idx++;
			if (prev) {
				this_->str=attr->u.str=g_strdup_printf("prev street_name_systematic:%s", prev->way.name_systematic);
				return 1;
			}
		case 5:
			this_->debug_idx++;
			if (prev) {
				this_->str=attr->u.str=g_strdup_printf("prev angle:(%d -) %d", prev->way.angle2, prev->angle_end);
				return 1;
			}
		case 6:
			this_->debug_idx++;
			this_->ways=itm->way.next;
			if (prev) {
				this_->str=attr->u.str=g_strdup_printf("prev item type:%s", item_to_name(prev->way.item.type));
				return 1;
			}
		case 7:
			if (this_->ways && prev) {
				this_->str=attr->u.str=g_strdup_printf("other item angle:%d delta:%d flags:%d dir:%d type:%s id:(0x%x,0x%x)", this_->ways->angle2, angle_delta(prev->angle_end, this_->ways->angle2), this_->ways->flags, this_->ways->dir, item_to_name(this_->ways->item.type), this_->ways->item.id_hi, this_->ways->item.id_lo);
				this_->ways=this_->ways->next;
				return 1;
			}
			this_->debug_idx++;
		case 8:
			this_->debug_idx++;
			if (prev) {
				char *reason=NULL;
				maneuver_required2(this_->nav, prev, itm, &reason);
				this_->str=attr->u.str=g_strdup_printf("reason:%s",reason); //FIXME: we now have a struct
				return 1;
			}
			
		default:
			this_->attr_next=attr_none;
			return 0;
		}
	case attr_any:
		while (this_->attr_next != attr_none) {
			if (navigation_map_item_attr_get(priv_data, this_->attr_next, attr))
				return 1;
		}
		return 0;
	default:
		attr->type=attr_none;
		return 0;
	}
}

static void
navigation_map_item_attr_rewind(void *priv_data)
{
	struct map_rect_priv *priv = priv_data;
	priv->debug_idx=0;
	priv->attr_next=attr_navigation_short;
}

static struct item_methods navigation_map_item_methods = {
	navigation_map_item_coord_rewind,
	navigation_map_item_coord_get,
	navigation_map_item_attr_rewind,
	navigation_map_item_attr_get,
};


static void
navigation_map_destroy(struct map_priv *priv)
{
	g_free(priv);
}

static void
navigation_map_rect_init(struct map_rect_priv *priv)
{
	priv->cmd_next=priv->nav->cmd_first;
	priv->cmd_itm_next=priv->itm_next=priv->nav->first;
}

static struct map_rect_priv *
navigation_map_rect_new(struct map_priv *priv, struct map_selection *sel)
{
	struct navigation *nav=priv->navigation;
	struct map_rect_priv *ret=g_new0(struct map_rect_priv, 1);
	ret->nav=nav;
	navigation_map_rect_init(ret);
	ret->item.meth=&navigation_map_item_methods;
	ret->item.priv_data=ret;
#ifdef DEBUG
	ret->show_all=1;
#endif
	return ret;
}

static void
navigation_map_rect_destroy(struct map_rect_priv *priv)
{
	g_free(priv->str);
	g_free(priv);
}

/**
 * @brief Gets the next item from the navigation map.
 *
 * This function returns an item from a map rectangle on the navigation map and advances the item pointer,
 * so that at the next call the next item will be returned.
 *
 * The {@code type} member of the result, which indicates the type of maneuver, is generally copied over from
 * {@code maneuver->type}, though some exceptions apply: The first item in the map will have a type of
 * {@code nav_position} and the last one will have a type of {@code nav_destination}.
 * If {@code maneuver->merge_or_exit} indicates a merge or exit, the result will be of the corresponding
 * merge or exit type.
 *
 * Earlier versions of Navit had the entire logic for setting te maneuver type in this function, but this has
 * been moved to {@code command_new()} so that other functions can use the same results.
 *
 * @param priv The {@code struct map_rect_priv} of the map rect on the navigation map from which an item
 * is to be retrieved.
 *
 * @return The item, or NULL if there are no more items in the map rectangle
 */
static struct item *
navigation_map_get_item(struct map_rect_priv *priv)
{
	struct item *ret=&priv->item;
	if (!priv->itm_next)
		return NULL;
	priv->itm=priv->itm_next;
	priv->cmd=priv->cmd_next;
	priv->cmd_itm=priv->cmd_itm_next;
	if (!priv->cmd)
		return NULL;
	if (!priv->show_all && priv->itm->prev != NULL) 
		priv->itm=priv->cmd->itm;
	priv->itm_next=priv->itm->next;
	if (priv->itm->prev)
		ret->type=type_nav_none;
	else
		ret->type=type_nav_position;
	if (priv->cmd->itm == priv->itm) {
		priv->cmd_itm_next=priv->cmd->itm;
		priv->cmd_next=priv->cmd->next;
		if (priv->cmd_itm_next && !priv->cmd_itm_next->next)
			ret->type=type_nav_destination; /* FIXME: do we need to set that here? The generic case should catch that now... */
		else if (priv->cmd->maneuver && ((priv->cmd->maneuver->type != type_nav_none) || (priv->cmd->maneuver->merge_or_exit & (mex_merge | mex_exit)))) {
			/* if maneuver type or merge_or_exit is set, use these values */
			/* FIXME: make decision to use merge_or_exit context-dependent */
			switch (priv->cmd->maneuver->merge_or_exit) {
			case mex_merge_left:
				ret->type=type_nav_merge_left;
				break;
			case mex_merge_right:
				ret->type=type_nav_merge_right;
				break;
			case mex_exit_left:
				ret->type=type_nav_exit_left;
				break;
			case mex_exit_right:
				ret->type=type_nav_exit_right;
				break;
			default:
				/* exit or merge without a direction should never happen,
				 * mex_intersection results in a regular instruction,
				 * thus all these are handled by the default case,
				 * which is to return the type field */
				ret->type = priv->cmd->maneuver->type;
			}
		} /* else if priv->cmd->maneuver ... */
	} /* if priv->cmd->itm == priv->itm */
	navigation_map_item_coord_rewind(priv);
	navigation_map_item_attr_rewind(priv);

	ret->id_lo=priv->itm->dest_count;
	dbg(lvl_debug,"type=%d\n", ret->type);
	return ret;
}

/**
 * @brief Gets the item with the specified ID from the navigation map.
 *
 * This function returns the item with the ID specified in the arguments from a map rectangle on the
 * navigation map.
 *
 * Internally the function calls {@code navigation_map_get_item()}, thus the same logic applies for the
 * data of the item that is returned. See {@code navigation_map_get_item()} for details.
 *
 * The item pointer of the map rectangle will be moved so that a subsequent call to {@code navigation_map_get_item()}
 * will return the next item following the one returned by this function.
 *
 * @param priv The {@code struct map_rect_priv} of the map rect on the navigation map from which an item
 * is to be retrieved.
 * @param id_hi The high part of the ID
 * @param id_lo The low part of the IF
 *
 * @return The item, or NULL if an item with the ID specified was not found in the map rectangle
 */
static struct item *
navigation_map_get_item_byid(struct map_rect_priv *priv, int id_hi, int id_lo)
{
	struct item *ret;
	navigation_map_rect_init(priv);
	while ((ret=navigation_map_get_item(priv))) {
		if (ret->id_hi == id_hi && ret->id_lo == id_lo) 
			return ret;
	}
	return NULL;
}

static struct map_methods navigation_map_meth = {
	projection_mg,
	"utf-8",
	navigation_map_destroy,
	navigation_map_rect_new,
	navigation_map_rect_destroy,
	navigation_map_get_item,
	navigation_map_get_item_byid,
	NULL,
	NULL,
	NULL,
};

static struct map_priv *
navigation_map_new(struct map_methods *meth, struct attr **attrs, struct callback_list *cbl)
{
	struct map_priv *ret;
	struct attr *navigation_attr;

	navigation_attr=attr_search(attrs, NULL, attr_navigation);
	if (! navigation_attr)
		return NULL;
	ret=g_new0(struct map_priv, 1);
	*meth=navigation_map_meth;
	ret->navigation=navigation_attr->u.navigation;

	return ret;
}

void
navigation_set_route(struct navigation *this_, struct route *route)
{
	struct attr callback;
	if (!this_->route_cb)
		this_->route_cb=callback_new_attr_1(callback_cast(navigation_update), attr_route_status, this_);
	callback.type=attr_callback;
	callback.u.callback=this_->route_cb;
	if (this_->route)
		route_remove_attr(this_->route, &callback);
	this_->route=route;
	if (this_->route) {
		struct attr route_status;
		route_add_attr(this_->route, &callback);
		if (route_get_attr(this_->route, attr_route_status, &route_status, NULL))
			navigation_update(this_, this_->route, &route_status);
	}
}

void
navigation_init(void)
{
	plugin_register_map_type("navigation", navigation_map_new);
}

struct object_func navigation_func = {
	attr_navigation,
	(object_func_new)navigation_new,
	(object_func_get_attr)navigation_get_attr,
	(object_func_iter_new)navit_object_attr_iter_new,
	(object_func_iter_destroy)navit_object_attr_iter_destroy,
	(object_func_set_attr)navigation_set_attr,
	(object_func_add_attr)navit_object_add_attr,
	(object_func_remove_attr)navit_object_remove_attr,
	(object_func_init)NULL,
	(object_func_destroy)navigation_destroy,
	(object_func_dup)NULL,
	(object_func_ref)navit_object_ref,
	(object_func_unref)navit_object_unref,
};

