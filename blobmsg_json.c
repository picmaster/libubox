/*
 * blobmsg - library for generating/parsing structured blob messages
 *
 * Copyright (C) 2010 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "blobmsg.h"
#include "blobmsg_json.h"

static bool blobmsg_add_object(struct blob_buf *b, json_object *obj)
{
	json_object_object_foreach(obj, key, val) {
		if (!blobmsg_add_json_element(b, key, val))
			return false;
	}
	return true;
}

static bool blobmsg_add_array(struct blob_buf *b, struct array_list *a)
{
	int i, len;

	for (i = 0, len = array_list_length(a); i < len; i++) {
		if (!blobmsg_add_json_element(b, NULL, array_list_get_idx(a, i)))
			return false;
	}

	return true;
}

bool blobmsg_add_json_element(struct blob_buf *b, const char *name, json_object *obj)
{
	bool ret = true;
	void *c;

	if (!obj)
		return false;

	switch (json_object_get_type(obj)) {
	case json_type_object:
		c = blobmsg_open_table(b, name);
		ret = blobmsg_add_object(b, obj);
		blobmsg_close_table(b, c);
		break;
	case json_type_array:
		c = blobmsg_open_array(b, name);
		ret = blobmsg_add_array(b, json_object_get_array(obj));
		blobmsg_close_array(b, c);
		break;
	case json_type_string:
		blobmsg_add_string(b, name, json_object_get_string(obj));
		break;
	case json_type_boolean:
		blobmsg_add_u8(b, name, json_object_get_boolean(obj));
		break;
	case json_type_int:
		blobmsg_add_u32(b, name, json_object_get_int(obj));
		break;
	default:
		return false;
	}
	return ret;
}

bool blobmsg_add_json_from_string(struct blob_buf *b, const char *str)
{
	json_object *obj;
	bool ret = false;

	obj = json_tokener_parse(str);
	if (is_error(obj))
		return false;

	if (json_object_get_type(obj) != json_type_object)
		goto out;

	ret = blobmsg_add_object(b, obj);

out:
	json_object_put(obj);
	return ret;
}


struct strbuf {
	int len;
	int pos;
	char *buf;

	blobmsg_json_format_t custom_format;
	void *priv;
};

static bool blobmsg_puts(struct strbuf *s, const char *c, int len)
{
	if (len <= 0)
		return true;

	if (s->pos + len >= s->len) {
		s->len += 16;
		s->buf = realloc(s->buf, s->len);
		if (!s->buf)
			return false;
	}
	memcpy(s->buf + s->pos, c, len);
	s->pos += len;
	return true;
}

static void blobmsg_format_string(struct strbuf *s, const char *str)
{
	const char *p, *last = str, *end = str + strlen(str);
	char buf[8] = "\\u00";

	blobmsg_puts(s, "\"", 1);
	for (p = str; *p; p++) {
		char escape = '\0';
		int len;

		switch(*p) {
		case '\b':
			escape = 'b';
			break;
		case '\n':
			escape = 'n';
			break;
		case '\t':
			escape = 't';
			break;
		case '\r':
			escape = 'r';
			break;
		case '"':
		case '\\':
		case '/':
			escape = *p;
			break;
		default:
			if (*p < ' ')
				escape = 'u';
			break;
		}

		if (!escape)
			continue;

		if (p > last)
			blobmsg_puts(s, last, p - last);
		last = p + 1;
		buf[1] = escape;

		if (escape == 'u') {
			sprintf(buf + 4, "%02x", (unsigned char) *p);
			len = 4;
		} else {
			len = 2;
		}
		blobmsg_puts(s, buf, len);
	}

	blobmsg_puts(s, last, end - last);
	blobmsg_puts(s, "\"", 1);
}

static void blobmsg_format_json_list(struct strbuf *s, struct blob_attr *attr, int len, bool array);

static void blobmsg_format_element(struct strbuf *s, struct blob_attr *attr, bool array, bool head)
{
	const char *data_str;
	char buf[32];
	void *data;
	int len;

	if (!blobmsg_check_attr(attr, false))
		return;

	if (!array && blobmsg_name(attr)[0]) {
		blobmsg_format_string(s, blobmsg_name(attr));
		blobmsg_puts(s, ":", 1);
	}
	if (head) {
		data = blob_data(attr);
		len = blob_len(attr);
	} else {
		data = blobmsg_data(attr);
		len = blobmsg_data_len(attr);

		if (s->custom_format) {
			data_str = s->custom_format(s->priv, attr);
			if (data_str)
				goto out;
		}
	}

	data_str = buf;
	switch(blob_id(attr)) {
	case BLOBMSG_TYPE_INT8:
		sprintf(buf, "%d", *(uint8_t *)data);
		break;
	case BLOBMSG_TYPE_INT16:
		sprintf(buf, "%d", *(uint16_t *)data);
		break;
	case BLOBMSG_TYPE_INT32:
		sprintf(buf, "%d", *(uint32_t *)data);
		break;
	case BLOBMSG_TYPE_INT64:
		sprintf(buf, "%lld", *(uint64_t *)data);
		break;
	case BLOBMSG_TYPE_STRING:
		blobmsg_format_string(s, data);
		return;
	case BLOBMSG_TYPE_ARRAY:
		blobmsg_format_json_list(s, data, len, true);
		return;
	case BLOBMSG_TYPE_TABLE:
		blobmsg_format_json_list(s, data, len, false);
		return;
	}

out:
	blobmsg_puts(s, data_str, strlen(data_str));
}

static void blobmsg_format_json_list(struct strbuf *s, struct blob_attr *attr, int len, bool array)
{
	struct blob_attr *pos;
	bool first = true;
	int rem = len;

	blobmsg_puts(s, (array ? "[ " : "{ "), 2);
	__blob_for_each_attr(pos, attr, rem) {
		if (!first)
			blobmsg_puts(s, ", ", 2);

		blobmsg_format_element(s, pos, array, false);
		first = false;
	}
	blobmsg_puts(s, (array ? " ]" : " }"), 2);
}

char *blobmsg_format_json_with_cb(struct blob_attr *attr, bool list, blobmsg_json_format_t cb, void *priv)
{
	struct strbuf s;

	s.len = blob_len(attr);
	s.buf = malloc(s.len);
	s.pos = 0;
	s.custom_format = cb;
	s.priv = priv;

	if (list)
		blobmsg_format_json_list(&s, blob_data(attr), blob_len(attr), false);
	else
		blobmsg_format_element(&s, attr, false, false);

	if (!s.len)
		return NULL;

	s.buf = realloc(s.buf, s.pos + 1);
	s.buf[s.pos] = 0;

	return s.buf;
}
