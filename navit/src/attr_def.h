/* prototypes */

/* common */
ATTR2(0x00000000,none)
ATTR(any)

ATTR2(0x00010000,type_item_begin)
ATTR(town_streets_item)
ATTR(street_name_item)
ATTR(street_name_numbers_item)
ATTR(street_item)
ATTR(street_number_item)
ATTR(item_type)
ATTR2(0x0001ffff,type_item_end)

ATTR2(0x00020000,type_int_begin)
ATTR(h)
ATTR(id)
ATTR(flags)
ATTR(w)
ATTR(x)
ATTR(y)
ATTR(flush_size)
ATTR(flush_time)
ATTR(zipfile_ref)
ATTR(country_id)
ATTR(position_sats)
ATTR(position_sats_used)
ATTR(update)
ATTR(follow)
ATTR2(0x00028000,type_boolean_begin)
/* boolean */
ATTR(overwrite)
ATTR(active)
ATTR2(0x0002ffff,type_int_end)
ATTR2(0x00030000,type_string_begin)
ATTR(type)
ATTR(label)
ATTR(data)
ATTR(charset)
ATTR(country_all)
ATTR(country_iso3)
ATTR(country_iso2)
ATTR(country_car)
ATTR(country_name)
ATTR(town_name)
ATTR(town_postal)
ATTR(district_name)
ATTR(street_name)
ATTR(street_name_systematic)
ATTR(street_number)
ATTR(debug)
ATTR(address)
ATTR(phone)
ATTR(entry_fee)
ATTR(open_hours)
ATTR(skin)
ATTR(fullscreen)
ATTR(view_mode)
ATTR(tilt)
ATTR(media_window_title)
ATTR(media_cmd)
/* poi */
ATTR(icon)
ATTR(info_html)
ATTR(price_html)
/* navigation */
ATTR(navigation_short)
ATTR(navigation_long)
ATTR(navigation_long_exact)
ATTR(navigation_speech)
ATTR(name)
ATTR(source)
ATTR2(0x0003ffff,type_string_end)
ATTR(order_limit)
ATTR2(0x00050000,type_double_start)
ATTR(position_height)
ATTR(position_speed)
ATTR(position_direction)
ATTR2(0x0005ffff,type_double_end)
ATTR2(0x00060000,type_coord_geo_start)
ATTR(position_coord_geo)
ATTR2(0x0006ffff,type_coord_geo_end)
ATTR2(0x00070000,type_color_begin)
ATTR(color)
ATTR2(0x0007ffff,type_color_end)
ATTR2(0x00080000,type_object_begin)
ATTR(navit)
ATTR(log)
ATTR(callback)
ATTR2(0x0008ffff,type_object_end)
