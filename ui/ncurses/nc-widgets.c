/*
 *  Copyright (C) 2013 IBM Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <linux/input.h> /* This must be included before ncurses.h */
#if defined HAVE_NCURSESW_CURSES_H
#  include <ncursesw/curses.h>
#elif defined HAVE_NCURSESW_H
#  include <ncursesw.h>
#elif defined HAVE_NCURSES_CURSES_H
#  include <ncurses/curses.h>
#elif defined HAVE_NCURSES_H
#  include <ncurses.h>
#elif defined HAVE_CURSES_H
#  include <curses.h>
#else
#  error "Curses header file not found."
#endif

#if defined HAVE_NCURSESW_FORM_H
#  include <ncursesw/form.h>
#elif defined HAVE_NCURSES_FORM_H
#  include <ncurses/form.h>
#elif defined HAVE_FORM_H
#  include <form.h>
#else
#  error "Curses form.h not found."
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <talloc/talloc.h>
#include <types/types.h>
#include <log/log.h>
#include <util/util.h>
#include <i18n/i18n.h>
#include <fold/fold.h>
#include <url/url.h>

#include "nc-cui.h"
#include "nc-widgets.h"

#undef move

#define to_checkbox(w) container_of(w, struct nc_widget_checkbox, widget)
#define to_textbox(w) container_of(w, struct nc_widget_textbox, widget)
#define to_button(w) container_of(w, struct nc_widget_button, widget)
#define to_select(w) container_of(w, struct nc_widget_select, widget)
#define to_subset(w) container_of(w, struct nc_widget_subset, widget)

static const char *checkbox_checked_str = "[*]";
static const char *checkbox_unchecked_str = "[ ]";

static const char *select_selected_str = "(*)";
static const char *select_unselected_str = "( )";

struct nc_widgetset {
	WINDOW	*mainwin;
	WINDOW	*subwin;
	FORM	*form;
	FIELD	**fields;
	int	n_fields, n_alloc_fields;
	void	(*widget_focus)(struct nc_widget *, void *);
	void	*widget_focus_arg;
	FIELD	*cur_field;

	/* custom validators */
	FIELDTYPE *ip_multi_type;
	FIELDTYPE *ip_type;
	FIELDTYPE *url_type;
};

struct nc_widget {
	FIELD	*field;
	bool	(*process_key)(struct nc_widget *, FORM *, int);
	void	(*set_visible)(struct nc_widget *, bool);
	void	(*move)(struct nc_widget *, int, int);
	void	(*field_focus)(struct nc_widget *, FIELD *);
	int	focussed_attr;
	int	unfocussed_attr;
	int	height;
	int	width;
	int	focus_y;
	int	x;
	int	y;
};

struct nc_widget_label {
	struct nc_widget	widget;
	const char		*text;
};

struct nc_widget_checkbox {
	struct nc_widget	widget;
	bool			checked;
};

struct nc_widget_textbox {
	struct nc_widgetset	*set;
	struct nc_widget	widget;
};

struct nc_widget_subset {
	struct nc_widget	widget;
	int			*active;
	int			n_active;
	struct subset_option {
		char		*str;
		int		val;
		FIELD		*field;
	} *options;
	int			n_options;
	int			top, left, size;
	struct nc_widgetset	*set;
	void			(*on_change)(void *, int);
	void			*on_change_arg;
	void			(*screen_cb)(void *,
					struct nc_widget_subset *, int);
};

struct nc_widget_select {
	struct nc_widget	widget;
	struct select_option {
		char		*str;
		int		val;
		FIELD		*field;
		int		lines;
	} *options;
	int			top, left, size;
	int			n_options, selected_option;
	struct nc_widgetset	*set;
	void			(*on_change)(void *, int);
	void			*on_change_arg;
};

struct nc_widget_button {
	struct nc_widget	widget;
	void			(*click)(void *arg);
	void			*arg;
};

static void widgetset_add_field(struct nc_widgetset *set, FIELD *field);
static void widgetset_remove_field(struct nc_widgetset *set, FIELD *field);

static bool key_is_select(int key)
{
	return key == ' ' || key == '\r' || key == '\n' || key == KEY_ENTER;
}

static bool process_key_nop(struct nc_widget *widget __attribute__((unused)),
		FORM *form __attribute((unused)),
		int key __attribute__((unused)))
{
	return false;
}

static void field_set_visible(FIELD *field, bool visible)
{
	int opts = field_opts(field) & ~O_VISIBLE;
	if (visible)
		opts |= O_VISIBLE;
	set_field_opts(field, opts);
}

static bool field_visible(FIELD *field)
{
	return (field_opts(field) & O_VISIBLE) == O_VISIBLE;
}

static void field_move(FIELD *field, int y, int x)
{
	move_field(field, y, x);
}

static int label_destructor(void *ptr)
{
	struct nc_widget_label *label = ptr;
	free_field(label->widget.field);
	return 0;
}


struct nc_widget_label *widget_new_label(struct nc_widgetset *set,
		int y, int x, char *str)
{
	struct nc_widget_label *label;
	FIELD *f;
	int len;

	len = strlen(str);

	label = talloc_zero(set, struct nc_widget_label);
	label->widget.height = 1;
	label->widget.width = len;
	label->widget.x = x;
	label->widget.y = y;
	label->widget.process_key = process_key_nop;
	label->widget.field = f = new_field(1, len, y, x, 0, 0);
	label->widget.focussed_attr = A_NORMAL;
	label->widget.unfocussed_attr = A_NORMAL;

	field_opts_off(f, O_ACTIVE);
	set_field_buffer(f, 0, str);
	set_field_userptr(f, &label->widget);

	widgetset_add_field(set, label->widget.field);
	talloc_set_destructor(label, label_destructor);

	return label;
}

bool widget_checkbox_get_value(struct nc_widget_checkbox *checkbox)
{
	return checkbox->checked;
}

static void checkbox_set_buffer(struct nc_widget_checkbox *checkbox)
{
	const char *str;
	str = checkbox->checked ? checkbox_checked_str : checkbox_unchecked_str;
	set_field_buffer(checkbox->widget.field, 0, str);
}

static bool checkbox_process_key(struct nc_widget *widget,
		FORM *form __attribute__((unused)), int key)
{
	struct nc_widget_checkbox *checkbox = to_checkbox(widget);

	if (!key_is_select(key))
		return false;

	checkbox->checked = !checkbox->checked;
	checkbox_set_buffer(checkbox);

	return true;
}

static int checkbox_destructor(void *ptr)
{
	struct nc_widget_checkbox *checkbox = ptr;
	free_field(checkbox->widget.field);
	return 0;
}

struct nc_widget_checkbox *widget_new_checkbox(struct nc_widgetset *set,
		int y, int x, bool checked)
{
	struct nc_widget_checkbox *checkbox;
	FIELD *f;

	checkbox = talloc_zero(set, struct nc_widget_checkbox);
	checkbox->checked = checked;
	checkbox->widget.height = 1;
	checkbox->widget.width = strlen(checkbox_checked_str);
	checkbox->widget.x = x;
	checkbox->widget.y = y;
	checkbox->widget.process_key = checkbox_process_key;
	checkbox->widget.focussed_attr = A_REVERSE;
	checkbox->widget.unfocussed_attr = A_NORMAL;
	checkbox->widget.field = f = new_field(1, strlen(checkbox_checked_str),
			y, x, 0, 0);

	field_opts_off(f, O_EDIT);
	set_field_userptr(f, &checkbox->widget);
	checkbox_set_buffer(checkbox);

	widgetset_add_field(set, checkbox->widget.field);
	talloc_set_destructor(checkbox, checkbox_destructor);

	return checkbox;
}

static char *strip_string(char *str)
{
	int len, i;

	len = strlen(str);

	/* clear trailing space */
	for (i = len - 1; i >= 0; i--) {
		if (!isspace(str[i]))
			break;
		str[i] = '\0';
	}

	/* increment str past leading space */
	for (i = 0; i < len; i++) {
		if (str[i] == '\0' || !isspace(str[i]))
			break;
	}

	return str + i;
}

char *widget_textbox_get_value(struct nc_widget_textbox *textbox)
{
	char *str = field_buffer(textbox->widget.field, 0);
	return str ? strip_string(str) : NULL;
}

static bool textbox_process_key(
		struct nc_widget *widget __attribute__((unused)),
		FORM *form, int key)
{
	switch (key) {
	case KEY_HOME:
		form_driver(form, REQ_BEG_FIELD);
		break;
	case KEY_END:
		form_driver(form, REQ_END_FIELD);
		break;
	case KEY_LEFT:
		form_driver(form, REQ_LEFT_CHAR);
		break;
	case KEY_RIGHT:
		form_driver(form, REQ_RIGHT_CHAR);
		break;
	case KEY_BACKSPACE:
		if (form_driver(form, REQ_LEFT_CHAR) != E_OK)
			break;
		/* fall through */
	case KEY_DC:
		form_driver(form, REQ_DEL_CHAR);
		break;
	case '\t':
	case KEY_BTAB:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_PPAGE:
	case KEY_NPAGE:
		/* Don't catch navigational keys */
		return false;
	default:
		form_driver(form, key);
		break;
	}

	return true;
}

static int textbox_destructor(void *ptr)
{
	struct nc_widget_textbox *textbox = ptr;
	free_field(textbox->widget.field);
	return 0;
}

struct nc_widget_textbox *widget_new_textbox(struct nc_widgetset *set,
		int y, int x, int len, char *str)
{
	struct nc_widget_textbox *textbox;
	FIELD *f;

	textbox = talloc_zero(set, struct nc_widget_textbox);
	textbox->set = set;
	textbox->widget.height = 1;
	textbox->widget.width = len;
	textbox->widget.x = x;
	textbox->widget.y = y;
	textbox->widget.process_key = textbox_process_key;
	textbox->widget.field = f = new_field(1, len, y, x, 0, 0);
	textbox->widget.focussed_attr = A_REVERSE;
	textbox->widget.unfocussed_attr = A_UNDERLINE;

	field_opts_off(f, O_STATIC | O_WRAP | O_BLANK);
	set_field_buffer(f, 0, str);
	set_field_back(f, textbox->widget.unfocussed_attr);
	set_field_userptr(f, &textbox->widget);

	widgetset_add_field(set, textbox->widget.field);
	talloc_set_destructor(textbox, textbox_destructor);

	return textbox;
}

void widget_textbox_set_fixed_size(struct nc_widget_textbox *textbox)
{
	field_opts_on(textbox->widget.field, O_STATIC);
}

void widget_textbox_set_validator_integer(struct nc_widget_textbox *textbox,
		long min, long max)
{
	set_field_type(textbox->widget.field, TYPE_INTEGER, 1, min, max);
}

static bool check_url_field(FIELD *field,
		const void *arg __attribute__((unused)))
{
	return is_url(field_buffer(field, 0));
}

void widget_textbox_set_validator_url(struct nc_widget_textbox *textbox)
{
	if (!textbox->set->url_type)
		textbox->set->url_type = new_fieldtype(check_url_field, NULL);

	set_field_type(textbox->widget.field, textbox->set->url_type);
}

static bool check_ip_field(FIELD *field,
		const void *arg __attribute__((unused)))
{
	char *str;
	int rc;

	str = strip_string(field_buffer(field, 0));

	rc = addr_scheme(str);

	return (rc == AF_INET || rc == AF_INET6);
}


static bool check_ipv6_multi_char(int c)
{
	return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
	       c == ':' || c == ' ';
}

static bool check_ipv4_multi_char(int c)
{
	return isdigit(c) || c == '.' || c == ' ';
}

static bool check_ip_multi_field(FIELD *field,
		const void *arg __attribute__((unused)))
{
	char *buf, *tok, *saveptr;
	bool result;
	int type;

	/* Use strdup directly since we can't easily reach a talloc parent */
	buf = strdup(strip_string(field_buffer(field, 0)));
	if (!buf)
		/* We tried */
		return true;

	result = false;
	tok = strtok_r(buf, " ", &saveptr);
	if (!tok || *tok == '\0')
		goto err;

	while (tok) {
		type = addr_scheme(tok);
		if (!(type == AF_INET || type == AF_INET6))
			goto err;

		tok = strtok_r(NULL, " ", &saveptr);
	}
	result = true;

err:
	free(buf);
	return result;
}

static bool check_ip_multi_char(int c, const void *arg __attribute__((unused)))
{
	return (check_ipv4_multi_char(c) || check_ipv6_multi_char(c));
}

void widget_textbox_set_validator_ip(struct nc_widget_textbox *textbox)
{
	if (!textbox->set->ip_type) {
		textbox->set->ip_type = new_fieldtype(check_ip_field, NULL);
	}
	set_field_type(textbox->widget.field, textbox->set->ip_type);
}

/*
 * In a perfect world we would use link_fieldtype() but segfaults do not
 * enhance the user experience.
 */
void widget_textbox_set_validator_ip_multi(struct nc_widget_textbox *textbox)
{
	if (!textbox->set->ip_multi_type) {
		textbox->set->ip_multi_type = new_fieldtype(
						check_ip_multi_field,
						check_ip_multi_char);
	}
	set_field_type(textbox->widget.field, textbox->set->ip_multi_type);
}

static void subset_update_order(struct nc_widget_subset *subset)
{
	char *str;
	int i, val;

	for (i = 0; i < subset->n_active; i++) {
		val = subset->active[i];
		str = talloc_asprintf(subset, "(%d) %s",
				      i, subset->options[val].str);
		set_field_buffer(subset->options[val].field, 0,
				 str);
		talloc_free(str);
	}
}

static void widget_focus_change(struct nc_widget *widget, FIELD *field,
		bool focussed);

static void subset_delete_active(struct nc_widget_subset *subset, int idx)
{
	bool last = idx == (subset->n_active - 1);
	struct nc_widgetset *set = subset->set;
	struct nc_widget *widget;
	size_t rem;
	int i, val;
	uint32_t opts;

	/* Shift field focus to nearest active option or next visible field */
	if (subset->n_active > 1) {
		if (last)
			val = subset->active[idx - 1];
		else
			val = subset->active[idx + 1];
		set->cur_field = subset->options[val].field;
	} else {
		for (i = 0; i < set->n_fields; i++)
			if (field_visible(set->fields[i])) {
				opts = field_opts(set->fields[i]);
				if ((opts & O_ACTIVE) == O_ACTIVE) {
					set->cur_field = set->fields[i];
					break;
				}
			}
	}

	set_current_field(set->form, set->cur_field);
	widget = field_userptr(set->cur_field);
	widget_focus_change(widget, set->cur_field, true);
	if (set->widget_focus)
		set->widget_focus(widget, set->widget_focus_arg);

	/* Update active array */
	rem = sizeof(int) * (subset->n_active - idx - 1);
	val = subset->active[idx];
	field_set_visible(subset->options[val].field, false);
	if (rem)
		memmove(&subset->active[idx], &subset->active[idx + 1], rem);
	subset->n_active--;
	subset->active = talloc_realloc(subset, subset->active,
					 int, subset->n_active);

	subset->widget.height = subset->n_active;
}

static bool subset_process_key(struct nc_widget *w, FORM *form, int key)
{
	struct nc_widget_subset *subset = to_subset(w);
	int i, val, opt_idx = -1;
	FIELD *field;

	if (key != '-' && key != '+' && key != KEY_DC && key != KEY_BACKSPACE)
		return false;

	field = current_field(form);

	for (i = 0; i < subset->n_active; i++) {
		val = subset->active[i];
		if (subset->options[val].field == field) {
			opt_idx = i;
			break;
		}
	}

	if (opt_idx < 0)
		return false;

	if (key == KEY_DC || key == KEY_BACKSPACE)
		subset_delete_active(subset, opt_idx);

	if (key == '-') {
		if (opt_idx == 0)
			return true;

		val = subset->active[opt_idx];
		subset->active[opt_idx] = subset->active[opt_idx - 1];
		subset->active[opt_idx - 1] = val;
	}

	if (key == '+') {
		if (opt_idx >= subset->n_active - 1)
			return true;

		val = subset->active[opt_idx];
		subset->active[opt_idx] = subset->active[opt_idx + 1];
		subset->active[opt_idx + 1] = val;
	}

	subset_update_order(subset);

	if (subset->on_change)
		subset->on_change(subset->on_change_arg, 0);

	return true;
}

static void subset_set_visible(struct nc_widget *widget, bool visible)
{
	struct nc_widget_subset *subset = to_subset(widget);
	int i, val;

	for (i = 0; i < subset->n_active; i++) {
		val = subset->active[i];
		field_set_visible(subset->options[val].field, visible);
	}
}

static void subset_move(struct nc_widget *widget, int y, int x)
{
	struct nc_widget_subset *subset = to_subset(widget);
	int i, val;

	for (i = 0; i < subset->n_active; i++) {
		val = subset->active[i];
		field_move(subset->options[val].field, y + i , x);
	}
}

static void subset_field_focus(struct nc_widget *widget, FIELD *field)
{
	struct nc_widget_subset *subset = to_subset(widget);
	int i, val;

	for (i = 0; i < subset->n_active; i++) {
		val = subset->active[i];
		if (field == subset->options[val].field) {
			widget->focus_y = i;
			return;
		}
	}
}

static int subset_destructor(void *ptr)
{
	struct nc_widget_subset *subset = ptr;
	int i;

	for (i = 0; i < subset->n_options; i++)
		free_field(subset->options[i].field);

	return 0;
}

struct nc_widget_subset *widget_new_subset(struct nc_widgetset *set,
		int y, int x, int len, void *screen_cb)
{
	struct nc_widget_subset *subset;

	subset = talloc_zero(set, struct nc_widget_subset);
	subset->widget.width = len;
	subset->widget.height = 0;
	subset->widget.x = x;
	subset->widget.y = y;
	subset->widget.process_key = subset_process_key;
	subset->widget.set_visible = subset_set_visible;
	subset->widget.move = subset_move;
	subset->widget.field_focus = subset_field_focus;
	subset->widget.focussed_attr = A_REVERSE;
	subset->widget.unfocussed_attr = A_NORMAL;
	subset->top = y;
	subset->left = x;
	subset->size = len;
	subset->set = set;
	subset->n_active = subset->n_options = 0;
	subset->active = NULL;
	subset->options = NULL;
	subset->screen_cb = screen_cb;

	talloc_set_destructor(subset, subset_destructor);

	return subset;
}

void widget_subset_add_option(struct nc_widget_subset *subset, const char *text)
{
	FIELD *f;
	int i;

	i = subset->n_options++;
	subset->options = talloc_realloc(subset, subset->options,
					 struct subset_option, i + 1);

	subset->options[i].str = talloc_strdup(subset->options, text);

	subset->options[i].field = f = new_field(1, subset->size, subset->top + i,
						 subset->left, 0, 0);

	field_opts_off(f, O_WRAP | O_EDIT);
	set_field_userptr(f, &subset->widget);
	set_field_buffer(f, 0, subset->options[i].str);
	field_set_visible(f, false);
	widgetset_add_field(subset->set, f);
}

void widget_subset_make_active(struct nc_widget_subset *subset, int idx)
{
	int i;

	for (i = 0; i < subset->n_active; i++)
		if (subset->active[i] == idx) {
			pb_debug("%s: Index %d already active\n", __func__, idx);
			return;
		}

	i = subset->n_active++;
	subset->widget.height = subset->n_active;
	subset->active = talloc_realloc(subset, subset->active,
					int, i + 1);
	subset->active[i] = idx;

	subset_update_order(subset);
}

int widget_subset_get_order(void *ctx, unsigned int **order,
		struct nc_widget_subset *subset)
{
	unsigned int *buf = talloc_array(ctx, unsigned int, subset->n_active);
	int i;

	for (i = 0; i < subset->n_active; i++)
		buf[i] = subset->active[i];

	*order = buf;
	return i;
}

void widget_subset_show_inactive(struct nc_widget_subset *subset,
		struct nc_widget_select *select)
{
	bool active = false, first = true;
	int i, j;

	for (i = 0; i < subset->n_options; i++) {
		active = false;
		for (j = 0; j < subset->n_active; j++)
			if (subset->active[j] == i)
				active = true;

		if (active)
			continue;

		widget_select_add_option(select, i,
					 subset->options[i].str, first);
		if (first)
			first = false;
	}
}

int widget_subset_n_inactive(struct nc_widget_subset *subset)
{
	return subset->n_options - subset->n_active;
}

int widget_subset_height(struct nc_widget_subset *subset)
{
	return subset->n_active;
}

void widget_subset_on_change(struct nc_widget_subset *subset,
		void (*on_change)(void *, int), void *arg)
{
	subset->on_change = on_change;
	subset->on_change_arg = arg;
}

void widget_subset_drop_options(struct nc_widget_subset *subset)
{
	struct nc_widgetset *set = subset->set;
	int i;

	for (i = 0; i < subset->n_options; i++) {
		FIELD *field = subset->options[i].field;
		widgetset_remove_field(set, field);
		if (field == set->cur_field)
			set->cur_field = NULL;
		free_field(subset->options[i].field);
	}

	talloc_free(subset->options);
	talloc_free(subset->active);
	subset->options = NULL;
	subset->active = NULL;
	subset->n_options = 0;
	subset->n_active = 0;
	subset->widget.height = 0;
	subset->widget.focus_y = 0;
}

void widget_subset_clear_active(struct nc_widget_subset *subset)
{
	int i;

	for (i = 0; i < subset->n_options; i++)
		field_set_visible(subset->options[i].field, false);

	talloc_free(subset->active);
	subset->active = NULL;
	subset->n_active = 0;
	subset->widget.height = 0;
	subset->widget.focus_y = 0;
}

void widget_subset_callback(void *arg,
		struct nc_widget_subset *subset, int idx)
{
	subset->screen_cb(arg, subset, idx);
}

static void select_option_change(struct select_option *opt, bool selected)
{
	const char *str;

	str = selected ? select_selected_str : select_unselected_str;

	memcpy(opt->str, str, strlen(str));
	set_field_buffer(opt->field, 0, opt->str);
}

static bool select_process_key(struct nc_widget *w, FORM *form, int key)
{
	struct nc_widget_select *select = to_select(w);
	struct select_option *new_opt, *old_opt;
	int i, new_idx;
	FIELD *field;

	if (!key_is_select(key))
		return false;

	field = current_field(form);
	new_opt = NULL;

	for (i = 0; i < select->n_options; i++) {
		if (select->options[i].field == field) {
			new_opt = &select->options[i];
			new_idx = i;
			break;
		}
	}

	if (!new_opt)
		return true;

	if (new_idx == select->selected_option)
		return true;

	old_opt = &select->options[select->selected_option];

	select_option_change(old_opt, false);
	select_option_change(new_opt, true);

	select->selected_option = new_idx;

	if (select->on_change)
		select->on_change(select->on_change_arg, new_opt->val);

	return true;
}

static void select_set_visible(struct nc_widget *widget, bool visible)
{
	struct nc_widget_select *select = to_select(widget);
	int i;

	for (i = 0; i < select->n_options; i++)
		field_set_visible(select->options[i].field, visible);
}

static void select_move(struct nc_widget *widget, int y, int x)
{
	struct nc_widget_select *select = to_select(widget);
	int i, cur = 0;

	for (i = 0; i < select->n_options; i++) {
		field_move(select->options[i].field, y + cur, x);
		cur += select->options[i].lines;
	}
}

static void select_field_focus(struct nc_widget *widget, FIELD *field)
{
	struct nc_widget_select *select = to_select(widget);
	int i, cur = 0;

	for (i = 0; i < select->n_options; i++) {
		if (field != select->options[i].field) {
			cur += select->options[i].lines;
			continue;
		}
		widget->focus_y = cur;
		return;
	}
}

static int select_destructor(void *ptr)
{
	struct nc_widget_select *select = ptr;
	int i;

	for (i = 0; i < select->n_options; i++)
		free_field(select->options[i].field);

	return 0;
}

struct nc_widget_select *widget_new_select(struct nc_widgetset *set,
		int y, int x, int len)
{
	struct nc_widget_select *select;

	select = talloc_zero(set, struct nc_widget_select);
	select->widget.width = len;
	select->widget.height = 0;
	select->widget.x = x;
	select->widget.y = y;
	select->widget.process_key = select_process_key;
	select->widget.set_visible = select_set_visible;
	select->widget.move = select_move;
	select->widget.field_focus = select_field_focus;
	select->widget.focussed_attr = A_REVERSE;
	select->widget.unfocussed_attr = A_NORMAL;
	select->top = y;
	select->left = x;
	select->size = len;
	select->set = set;

	talloc_set_destructor(select, select_destructor);

	return select;
}

static int widget_select_fold_cb(void *arg, const char *buf, int len)
{
	struct nc_widget_select *select = arg;
	char *line, *newstr, *padbuf = NULL;
	int i, pad;

	if (!len)
		return 0;

	line = talloc_strndup(select->options, buf, len);

	i = select->n_options - 1;
	pad = max(0, select->widget.width - strncols(line));

	if (pad) {
		padbuf = talloc_array(select->options, char, pad + 1);
		memset(padbuf, ' ', pad);
		padbuf[pad] = '\0';
	}

	if (select->options[i].str)
		newstr = talloc_asprintf_append(select->options[i].str,
							 "%s%s", line,
							 pad ? padbuf : "");
	else
		newstr = talloc_asprintf(select->options, "%s%s", line,
							 pad ? padbuf : "");

	select->options[i].str = newstr;
	select->options[i].lines++;

	talloc_free(padbuf);
	talloc_free(line);
	return 0;
}

void widget_select_add_option(struct nc_widget_select *select, int value,
		const char *text, bool selected)
{
	const char *str;
	char *full_text;
	FIELD *f;
	int i;

	/* if we never see an option with selected set, we want the first
	 * one to be selected */
	if (select->n_options == 0)
		selected = true;
	else if (selected)
		select_option_change(&select->options[select->selected_option],
					false);

	if (selected) {
		select->selected_option = select->n_options;
		str = select_selected_str;
	} else
		str = select_unselected_str;

	i = select->n_options++;
	select->options = talloc_realloc(select, select->options,
				struct select_option, i + 2);
	select->options[i].val = value;
	select->options[i].lines = 0;
	select->options[i].str = NULL;

	full_text = talloc_asprintf(select->options, "%s %s", str, text);
	fold_text(full_text, select->widget.width,
		  widget_select_fold_cb, select);

	select->options[i].field = f = new_field(select->options[i].lines,
					select->size,
					select->top + select->widget.height,
					select->left, 0, 0);

	select->widget.height += select->options[i].lines;

	field_opts_off(f, O_EDIT);
	set_field_userptr(f, &select->widget);
	set_field_buffer(f, 0, select->options[i].str);

	widgetset_add_field(select->set, f);
	talloc_free(full_text);
}

int widget_select_get_value(struct nc_widget_select *select)
{
	if (!select->n_options)
		return -1;
	return select->options[select->selected_option].val;
}

int widget_select_height(struct nc_widget_select *select)
{
	return select->widget.height;
}

void widget_select_on_change(struct nc_widget_select *select,
		void (*on_change)(void *, int), void *arg)
{
	select->on_change = on_change;
	select->on_change_arg = arg;
}

void widget_select_drop_options(struct nc_widget_select *select)
{
	struct nc_widgetset *set = select->set;
	int i;

	for (i = 0; i < select->n_options; i++) {
		FIELD *field = select->options[i].field;
		widgetset_remove_field(set, field);
		if (field == set->cur_field)
			set->cur_field = NULL;
		free_field(select->options[i].field);
	}

	talloc_free(select->options);
	select->options = NULL;
	select->n_options = 0;
	select->widget.height = 0;
	select->widget.focus_y = 0;

}

static bool button_process_key(struct nc_widget *widget,
		FORM *form __attribute__((unused)), int key)
{
	struct nc_widget_button *button = to_button(widget);

	if (!button->click)
		return false;

	if (!key_is_select(key))
		return false;

	button->click(button->arg);
	return true;
}

static int button_destructor(void *ptr)
{
	struct nc_widget_button *button = ptr;
	free_field(button->widget.field);
	return 0;
}

struct nc_widget_button *widget_new_button(struct nc_widgetset *set,
		int y, int x, int size, const char *str,
		void (*click)(void *), void *arg)
{
	struct nc_widget_button *button;
	int idx, len, pad1, pad2, bufsz;
	char *text;
	FIELD *f;

	int field_size = size + 2;

	button = talloc_zero(set, struct nc_widget_button);
	button->widget.height = 1;
	button->widget.width = field_size;
	button->widget.x = x;
	button->widget.y = y;
	button->widget.field = f = new_field(1, field_size, y, x, 0, 0);
	button->widget.process_key = button_process_key;
	button->widget.focussed_attr = A_REVERSE;
	button->widget.unfocussed_attr = A_NORMAL;
	button->click = click;
	button->arg = arg;

	field_opts_off(f, O_EDIT);
	set_field_userptr(f, &button->widget);

	/* Center str in the field. This depends on the number of columns used
	 * by the string, not the number of chars in str */
	len = strncols(str);
	if (len <= size) {
		idx = (field_size - len) / 2;
	} else {
		idx = 1;
		pb_log("Warning: '%s' %d columns wide "
		       "but button is %d columns wide\n",
		       str, len, size);
	}

	pad1 = max(idx - 1, 0);
	pad2 = max(size - len - pad1, 0);
	bufsz = 1 + pad1 + strlen(str) + pad2 + 2;

	text = talloc_array(button, char, bufsz);
	memset(text, ' ', bufsz);
	memcpy(text + idx, str, strlen(str));
	text[0] = '[';
	text[bufsz - 2] = ']';
	text[bufsz - 1] = '\0';

	set_field_buffer(f, 0, text);

	widgetset_add_field(set, button->widget.field);
	talloc_set_destructor(button, button_destructor);

	return button;
}

void widget_focus_change(struct nc_widget *widget, FIELD *field,
		bool focussed)
{
	int attr = focussed ? widget->focussed_attr : widget->unfocussed_attr;
	set_field_back(field, attr);
}

bool widgetset_process_key(struct nc_widgetset *set, int key)
{
	struct nc_widget *widget;
	FIELD *field, *tmp;
	int req = 0;
	bool tab;

	field = current_field(set->form);
	assert(field);

	widget = field_userptr(field);

	if (widget->process_key)
		if (widget->process_key(widget, set->form, key))
			return true;

	tab = false;

	/* handle field change events */
	switch (key) {
	case KEY_BTAB:
		tab = true;
		/* fall through */
	case KEY_UP:
		req = REQ_SPREV_FIELD;
		break;
	case '\t':
		tab = true;
		/* fall through */
	case KEY_DOWN:
		req = REQ_SNEXT_FIELD;
		break;
	case KEY_PPAGE:
		req = REQ_SFIRST_FIELD;
		break;
	case KEY_NPAGE:
		req = REQ_SLAST_FIELD;
		break;
	case KEY_LEFT:
		req = REQ_LEFT_FIELD;
		break;
	case KEY_RIGHT:
		req = REQ_RIGHT_FIELD;
		break;
	}

	if (req) {
		widget_focus_change(widget, field, false);
		form_driver(set->form, req);

		/* if we're doing a tabbed-field-change, skip until we
		 * see the next widget */
		tmp = field;
		field = current_field(set->form);

		for (; tab && tmp != field && field_userptr(field) == widget;) {
			form_driver(set->form, req);
			field = current_field(set->form);
		}

		form_driver(set->form, REQ_END_FIELD);
		widget = field_userptr(field);
		widget_focus_change(widget, field, true);
		if (widget->field_focus)
			widget->field_focus(widget, field);
		if (set->widget_focus)
			set->widget_focus(widget, set->widget_focus_arg);
		return true;
	}

	return false;
}

static int widgetset_destructor(void *ptr)
{
	struct nc_widgetset *set = ptr;
	free_form(set->form);
	if (set->ip_type)
		free_fieldtype(set->ip_type);
	if (set->ip_multi_type)
		free_fieldtype(set->ip_multi_type);
	if (set->url_type)
		free_fieldtype(set->url_type);
	return 0;
}

struct nc_widgetset *widgetset_create(void *ctx, WINDOW *main, WINDOW *sub)
{
	struct nc_widgetset *set;

	set = talloc_zero(ctx, struct nc_widgetset);
	set->n_alloc_fields = 8;
	set->mainwin = main;
	set->subwin = sub;
	set->fields = talloc_array(set, FIELD *, set->n_alloc_fields);
	talloc_set_destructor(set, widgetset_destructor);

	return set;
}

void widgetset_set_windows(struct nc_widgetset *set,
		WINDOW *main, WINDOW *sub)
{
	set->mainwin = main;
	set->subwin = sub;
}

void widgetset_set_widget_focus(struct nc_widgetset *set,
		widget_focus_cb cb, void *arg)
{
	set->widget_focus = cb;
	set->widget_focus_arg = arg;
}

void widgetset_post(struct nc_widgetset *set)
{
	struct nc_widget *widget;
	FIELD *field;

	set->form = new_form(set->fields);
	set_form_win(set->form, set->mainwin);
	set_form_sub(set->form, set->subwin);
	post_form(set->form);
	form_driver(set->form, REQ_END_FIELD);

	if (set->cur_field)
		set_current_field(set->form, set->cur_field);

	field = current_field(set->form);
	widget = field_userptr(field);
	widget_focus_change(widget, field, true);
	if (set->widget_focus)
		set->widget_focus(widget, set->widget_focus_arg);
}

void widgetset_unpost(struct nc_widgetset *set)
{
	set->cur_field = current_field(set->form);
	unpost_form(set->form);
	free_form(set->form);
	set->form = NULL;
}

static void widgetset_add_field(struct nc_widgetset *set, FIELD *field)
{
	if (set->n_fields == set->n_alloc_fields - 1) {
		set->n_alloc_fields *= 2;
		set->fields = talloc_realloc(set, set->fields,
				FIELD *, set->n_alloc_fields);
	}

	set->n_fields++;
	set->fields[set->n_fields - 1] = field;
	set->fields[set->n_fields] = NULL;
}

static void widgetset_remove_field(struct nc_widgetset *set, FIELD *field)
{
	int i;

	for (i = 0; i < set->n_fields; i++) {
		if (set->fields[i] == field)
			break;
	}

	if (i == set->n_fields)
		return;

	memmove(&set->fields[i], &set->fields[i+i],
			(set->n_fields - i) * sizeof(set->fields[i]));
	set->n_fields--;
}

#define DECLARE_BASEFN(type) \
	struct nc_widget *widget_ ## type ## _base		\
		(struct nc_widget_ ## type *w)			\
	{ return &w->widget; }

DECLARE_BASEFN(textbox);
DECLARE_BASEFN(checkbox);
DECLARE_BASEFN(subset);
DECLARE_BASEFN(select);
DECLARE_BASEFN(label);
DECLARE_BASEFN(button);

void widget_set_visible(struct nc_widget *widget, bool visible)
{
	if (widget->set_visible)
		widget->set_visible(widget, visible);
	else
		field_set_visible(widget->field, visible);
}

void widget_move(struct nc_widget *widget, int y, int x)
{
	if (widget->move)
		widget->move(widget, y, x);
	else
		field_move(widget->field, y, x);

	widget->x = x;
	widget->y = y;

	if (x + widget->width > COLS)
		pb_debug("%s: Widget at %d,%d runs over pad! (%d)", __func__,
		       y, x, x + widget->width);
}

int widget_height(struct nc_widget *widget)
{
	return widget->height;
}

int widget_width(struct nc_widget *widget)
{
	return widget->width;
}

int widget_x(struct nc_widget *widget)
{
	return widget->x;
}

int widget_y(struct nc_widget *widget)
{
	return widget->y;
}

int widget_focus_y(struct nc_widget *widget)
{
	return widget->focus_y;
}

