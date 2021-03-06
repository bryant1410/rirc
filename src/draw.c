/* draw.c
 *
 * Draw the elements in state.c to the terminal.
 *
 * Assumes vt-100 compatible escape codes, as such YMMV */

#include <alloca.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "state.h"
/* FIXME: this has to be included after common.h for activity cols */
#include "../config.h"

#define ESC "\x1b"

/* Set foreground/background colour */
#define FG(X) ESC"[38;5;"#X"m"
#define BG(X) ESC"[48;5;"#X"m"

/* Reset foreground/background colour */
#define FG_R ESC"[39m"
#define BG_R ESC"[49m"

#define CLEAR_ATTRIBUTES ESC"[0m"

#define MOVE(X, Y) ESC"["#X";"#Y"H"

#define CLEAR_FULL  ESC"[2J"
#define CLEAR_RIGHT ESC"[0K"
#define CLEAR_LEFT  ESC"[1K"
#define CLEAR_LINE  ESC"[2K"

/* Save and restore the cursor's location */
#define CURSOR_SAVE    ESC"[s"
#define CURSOR_RESTORE ESC"[u"

/* Minimum rows or columns to safely draw */
#define COLS_MIN 5
#define ROWS_MIN 5

#ifndef BUFFER_PADDING
	#define BUFFER_PADDING 1
#elif BUFFER_PADDING != 0 && BUFFER_PADDING != 1
	#error "BUFFER_PADDING options are 0 (no pad), 1 (padded)"
#endif

/* Terminal coordinate row/column boundaries (inclusive) for objects being drawn
 *
 *   \ c0     cN
 *    +---------+
 *  r0|         |
 *    |         |
 *    |         |
 *  rN|         |
 *    +---------+
 *
 * The origin for terminal coordinates is in the top left, indexed from 1
 *
 * */
struct coords
{
	unsigned int c1;
	unsigned int cN;
	unsigned int r1;
	unsigned int rN;
};

static union
{
	struct {
		#define X(bit) unsigned int bit : 1;
		DRAW_BITS
		#undef X
	} bits;
	unsigned int all_bits;
} _draw;

#define X(BIT) void draw_##BIT(void) { _draw.bits.BIT = 1; }
DRAW_BITS
#undef X

static int _draw_fmt(char*, size_t*, size_t*, size_t*, int, const char*, ...);

static void _draw_buffer_line(struct buffer_line*, struct coords, unsigned int, unsigned int, unsigned int, unsigned int);
static void _draw_buffer(struct buffer*, struct coords);
static void _draw_input(channel*);
static void _draw_nav(channel*);
static void _draw_status(channel*);

static inline unsigned int nick_col(char*);
static inline void check_coords(struct coords);

void
draw_all(void)
{
	/* Set all bits to be redrawn */

	_draw.all_bits = -1;
}

void
draw(void)
{
	if (!_draw.all_bits)
		return;

	channel *c = current_channel();

	if (_term_cols() < COLS_MIN || _term_rows() < ROWS_MIN) {
		printf(CLEAR_FULL MOVE(1, 1) "rirc");
		goto no_draw;
	}

	printf(CURSOR_SAVE);

	if (_draw.bits.buffer) _draw_buffer(&c->buffer,
		(struct coords) {
			.c1 = 1,
			.cN = _term_cols(),
			.r1 = 3,
			.rN = _term_rows() - 2
		});
	if (_draw.bits.nav)    _draw_nav(c);
	if (_draw.bits.input)  _draw_input(c);
	if (_draw.bits.status) _draw_status(c);

	printf(CLEAR_ATTRIBUTES);
	printf(CURSOR_RESTORE);

no_draw:

	fflush(stdout);

	_draw.all_bits = 0;
}

/* FIXME: works except when it doesn't.
 *
 * Fails when line headers are very long compared to text. tests/draw.c needed */
static void
_draw_buffer_line(
		struct buffer_line *line,
		struct coords coords,
		unsigned int head_w,
		unsigned int text_w,
		unsigned int skip,
		unsigned int pad)
{
	check_coords(coords);

	char *print_p1,
	     *print_p2,
	     *p1 = line->text,
	     *p2 = line->text + line->text_len;

	if (skip == 0) {

		/* Print the line header
		 *
		 * Since formatting codes don't occupy columns, enough space
		 * should be allocated for all such sequences
		 * */
		char header[head_w + sizeof(FG(255) BG(255)) * 4 + 1];

		struct tm *line_tm = localtime(&line->time);

		size_t buff_n = sizeof(header) - 1,
		       offset = 0,
		       head_n = head_w - 1;

		if (!_draw_fmt(header, &offset, &buff_n, &head_n, 0,
				FG(%d) BG_R, BUFFER_LINE_HEADER_FG_NEUTRAL))
			goto print_header;

		if (!_draw_fmt(header, &offset, &buff_n, &head_n, 1,
				" %02d:%02d ", line_tm->tm_hour, line_tm->tm_min))
			goto print_header;

		if (!_draw_fmt(header, &offset, &buff_n, &head_n, 1,
				"%*s", pad, ""))
			goto print_header;

		if (!_draw_fmt(header, &offset, &buff_n, &head_n, 0,
				FG_R BG_R))
			goto print_header;

		switch (line->type) {
			case BUFFER_LINE_OTHER:
				if (!_draw_fmt(header, &offset, &buff_n, &head_n, 0,
						FG(%d), BUFFER_LINE_HEADER_FG_NEUTRAL))
					goto print_header;
				break;

			case BUFFER_LINE_CHAT:
				if (!_draw_fmt(header, &offset, &buff_n, &head_n, 0,
						FG(%d), nick_col(line->from)))
					goto print_header;
				break;

			case BUFFER_LINE_PINGED:
				if (!_draw_fmt(header, &offset, &buff_n, &head_n, 0,
						FG(%d) BG(%d), BUFFER_LINE_HEADER_FG_PINGED, BUFFER_LINE_HEADER_BG_PINGED))
					goto print_header;
				break;

			case BUFFER_LINE_T_SIZE:
				break;
		}

		if (!_draw_fmt(header, &offset, &buff_n, &head_n, 1,
				"%s", line->from))
			goto print_header;

print_header:
		/* Print the line header */
		printf(MOVE(%d, 1) "%s " CLEAR_ATTRIBUTES, coords.r1, header);
	}

	while (skip--)
		word_wrap(text_w, &p1, p2);

	do {
		char *sep = " "VERTICAL_SEPARATOR" ";

		if ((coords.cN - coords.c1) >= sizeof(*sep) + text_w) {
			printf(MOVE(%d, %d), coords.r1, (int)(coords.cN - (sizeof(*sep) + text_w + 1)));
			printf(FG(%d) BG_R, BUFFER_LINE_HEADER_FG_NEUTRAL);
			puts(sep);
		}

		if (*p1) {
			printf(MOVE(%d, %d), coords.r1, head_w);

			print_p1 = p1;
			print_p2 = word_wrap(text_w, &p1, p2);

			printf(FG(%d) BG_R, line->text[0] == QUOTE_CHAR
				? BUFFER_LINE_TEXT_FG_GREEN
				: BUFFER_LINE_TEXT_FG_NEUTRAL);

			printf("%.*s", (int)(print_p2 - print_p1), print_p1);
		}

		coords.r1++;

	} while (*p1 && coords.r1 <= coords.rN);
}

static void
_draw_buffer(struct buffer *b, struct coords coords)
{
	/* Dynamically draw the current channel's buffer such that:
	 *
	 * - The scrollback line should always be drawn in full when possible
	 * - Lines wrap on whitespace when possible
	 * - The top-most lines draws partially when required
	 * - Buffers requiring fewer rows than available draw from the top down
	 *
	 * Rows are numbered from the top down, 1 to term_rows, so for term_rows = N,
	 * the drawable area for the buffer is bounded [r3, rN-2]:
	 *      __________________________
	 * r1   |         (nav)          |
	 * r2   |------------------------|
	 * r3   |    ::buffer start::    |
	 *      |                        |
	 * ...  |                        |
	 *      |                        |
	 * rN-2 |     ::buffer end::     |
	 * rN-1 |------------------------|
	 * rN   |________(input)_________|
	 *
	 *
	 * So the general steps for drawing are:
	 *
	 * 1. Starting from line L = scrollback, traverse backwards through the
	 *    buffer summing the rows required to draw lines, until the sum
	 *    exceeds the number of rows available
	 *
	 * 2. L now points to the top-most line to be drawn. L might not be able
	 *    to draw in full, so discard the excessive word-wrapped segments and
	 *    draw the remainder
	 *
	 * 3. Traverse forward through the buffer, drawing lines until buffer.head
	 *    is encountered
	 */

	check_coords(coords);

	unsigned int row,
	             row_count = 0,
	             row_total = coords.rN - coords.r1 + 1;

	unsigned int col_total = coords.cN - coords.c1 + 1;

	unsigned int buffer_i = b->scrollback,
	             head_w,
	             text_w;

	/* Clear the buffer area */
	for (row = coords.r1; row <= coords.rN; row++)
		printf(MOVE(%d, 1) CLEAR_LINE, row);

	struct buffer_line *line = buffer_line(b, buffer_i);

	if (line == NULL)
		return;

	struct buffer_line *tail = buffer_tail(b);
	struct buffer_line *head = buffer_head(b);

	/* Find top line */
	for (;;) {

		split_buffer_cols(line, NULL, &text_w, col_total, b->pad);

		row_count += buffer_line_rows(line, text_w);

		if (line == tail)
			break;

		if (row_count >= row_total)
			break;

		line = buffer_line(b, --buffer_i);
	}

	/* Handle impartial top line print */
	if (row_count > row_total) {

		split_buffer_cols(line, &head_w, &text_w, col_total, b->pad);

		_draw_buffer_line(
			line,
			coords,
			head_w,
			text_w,
			row_count - row_total,
			BUFFER_PADDING ? (b->pad - line->from_len) : 0
		);

		coords.r1 += buffer_line_rows(line, text_w) - (row_count - row_total);

		if (line == head)
			return;

		line = buffer_line(b, ++buffer_i);
	}

	/* Draw all remaining lines */
	while (coords.r1 <= coords.rN) {

		split_buffer_cols(line, &head_w, &text_w, col_total, b->pad);

		_draw_buffer_line(
			line,
			coords,
			head_w,
			text_w,
			0,
			BUFFER_PADDING ? (b->pad - line->from_len) : 0
		);

		coords.r1 += buffer_line_rows(line, text_w);

		if (line == head)
			return;

		line = buffer_line(b, ++buffer_i);
	}
}

/* TODO
 *
 * | [server-name[:port]] *#chan |
 *
 * - Disconnected/parted channels are printed (#chan)
 * - Servers with non-standard ports are printed: server-name:port
 * - Channels that won't fit are printed at a minimum: #...
 *     - eg: | ...chan #chan2 chan3 |   Right printing
 *           | #chan1 #chan2 #ch... |   Left printing
 * */
static void
_draw_nav(channel *c)
{
	/* Dynamically draw the nav such that:
	 *
	 *  - The current channel is kept framed while navigating
	 *  - Channels are coloured based on their current activity
	 *  - The nav is kept framed between the first and last channels
	 */

	printf(MOVE(1, 1) CLEAR_LINE);

	static channel *frame_prev, *frame_next;

	channel *tmp, *current = c;

	channel *c_first = channel_get_first();
	channel *c_last = channel_get_last();

	/* By default assume drawing starts towards the next channel */
	unsigned int nextward = 1;

	size_t len, total_len = 0;

	/* Bump the channel frames, if applicable */
	if ((total_len = (strlen(c->name) + 2)) >= _term_cols())
		return;
	else if (c == frame_prev && frame_prev != c_first)
		frame_prev = channel_get_prev(frame_prev);
	else if (c == frame_next && frame_next != c_last)
		frame_next = channel_get_next(frame_next);

	/* Calculate the new frames */
	channel *tmp_prev = c, *tmp_next = c;

	for (;;) {

		if (tmp_prev == c_first || tmp_prev == frame_prev) {

			/* Pad out nextward */

			tmp = channel_get_next(tmp_next);
			len = strlen(tmp->name);

			while ((total_len += (len + 2)) < _term_cols() && tmp != c_first) {

				tmp_next = tmp;

				tmp = channel_get_next(tmp);
				len = strlen(tmp->name);
			}

			break;
		}

		if (tmp_next == c_last || tmp_next == frame_next) {

			/* Pad out prevward */

			tmp = channel_get_prev(tmp_prev);
			len = strlen(tmp->name);

			while ((total_len += (len + 2)) < _term_cols() && tmp != c_last) {

				tmp_prev = tmp;

				tmp = channel_get_prev(tmp);
				len = strlen(tmp->name);
			}

			break;
		}


		tmp = nextward ? channel_get_next(tmp_next) : channel_get_prev(tmp_prev);
		len = strlen(tmp->name);

		/* Next channel doesn't fit */
		if ((total_len += (len + 2)) >= _term_cols())
			break;

		if (nextward)
			tmp_next = tmp;
		else
			tmp_prev = tmp;

		nextward = !nextward;
	}

	frame_prev = tmp_prev;
	frame_next = tmp_next;

	/* Draw coloured channel names, from frame to frame */
	for (c = frame_prev; ; c = channel_get_next(c)) {

		/* Set print colour and print name */
		if (printf(FG(%d), (c == current) ? 255 : actv_cols[c->active]) < 0)
			break;

		if (printf(" %s ", c->name) < 0)
			break;

		if (c == frame_next)
			break;
	}

	current->active = ACTIVITY_DEFAULT;
}

/* TODO:
 *
 * Could use some cleaning up*/
static void
_draw_input(channel *c)
{
	unsigned int cols = _term_cols();
	unsigned int rows = _term_rows();

	printf(MOVE(%d, 1), rows);
	printf(FG(%d) BG_R, BUFFER_LINE_HEADER_FG_NEUTRAL);
	printf("%.*s", cols, " >>> ");

	printf(CLEAR_ATTRIBUTES);

	/* Action messages override the input bar */
	if (action_message) {
		printf(CLEAR_RIGHT FG(%d) "%s",
				INPUT_FG_NEUTRAL, action_message);
		return;
	}

	unsigned int winsz = cols / 3;

	input *in = c->input;

	/* Reframe the input bar window */
	if (in->head > (in->window + cols - 6))
		in->window += winsz;
	else if (in->head == in->window - 1)
		in->window = (in->window - winsz > in->line->text)
			? in->window - winsz : in->line->text;

	printf(CLEAR_RIGHT FG(%d), INPUT_FG_NEUTRAL);

	char *p = in->window;
	while (p < in->head)
		putchar(*p++);

	p = in->tail;

	char *end = in->tail + cols - 5 - (in->head - in->window);

	while (p < end && p < in->line->text + MAX_INPUT)
		putchar(*p++);

	int col = (in->head - in->window);

	printf(MOVE(%d, %d), _term_rows(), col + 6);
	printf(CURSOR_SAVE);
}

static void
_draw_status(channel *c)
{
	/* server / private chat:
	 * |-[usermodes]-(latency)---...|
	 *
	 * channel:
	 * |-[usermodes]-[chancount chantype chanmodes]/[priv]-(latency)---...|
	 * */

	float sb;
	int ret;
	unsigned int col = 0;
	unsigned int cols = _term_cols();
	unsigned int rows = _term_rows();

	/* Insufficient columns for meaningful status */
	if (cols < 3)
		return;

	printf(CLEAR_ATTRIBUTES);

	printf(MOVE(2, 1));
	printf("%.*s", cols, (char *)(memset(alloca(cols), *HORIZONTAL_SEPARATOR, cols)));

	printf(MOVE(%d, 1) CLEAR_LINE, rows - 1);

	/* Print status to temporary buffer */
	char status_buff[cols + 1];

	memset(status_buff, 0, cols + 1);

	/* -[usermodes] */
	if (c->server && *c->server->usermodes) {
		ret = snprintf(status_buff + col, cols - col + 1, "%s", HORIZONTAL_SEPARATOR "[+");
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;

		ret = snprintf(status_buff + col, cols - col + 1, "%s", c->server->usermodes);
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;

		ret = snprintf(status_buff + col, cols - col + 1, "%s", "]");
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;
	}

	/* If private chat buffer:
	 * -[priv] */
	if (c->buffer.type == BUFFER_PRIVATE) {
		ret = snprintf(status_buff + col, cols - col + 1, "%s", HORIZONTAL_SEPARATOR "[priv]");
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;
	}

	/* If IRC channel buffer:
	 * -[chancount chantype chanmodes] */
	if (c->buffer.type == BUFFER_CHANNEL) {

		ret = snprintf(status_buff + col, cols - col + 1,
				HORIZONTAL_SEPARATOR "[%d", c->nick_count);
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;

		if (c->type_flag) {
			ret = snprintf(status_buff + col, cols - col + 1, " %c", c->type_flag);
			if (ret < 0 || (col += ret) >= cols)
				goto print_status;
		}

		if (*c->chanmodes) {
			ret = snprintf(status_buff + col, cols - col + 1, " +%s", c->chanmodes);
			if (ret < 0 || (col += ret) >= cols)
				goto print_status;
		}

		ret = snprintf(status_buff + col, cols - col + 1, "%s", "]");
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;
	}

	/* -(latency) */
	if (c->server && c->server->latency_delta) {
		ret = snprintf(status_buff + col, cols - col + 1,
				HORIZONTAL_SEPARATOR "(%llds)", (long long) c->server->latency_delta);
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;
	}

	/* -(scrollback%) */
	if ((sb = buffer_scrollback_status(&c->buffer))) {
		ret = snprintf(status_buff + col, cols - col + 1,
				HORIZONTAL_SEPARATOR "(%02d%%)", (int)(sb * 100));
		if (ret < 0 || (col += ret) >= cols)
			goto print_status;
	}

print_status:

	printf("%s", status_buff);

	/* Trailing separator */
	while (col++ < cols)
		printf(HORIZONTAL_SEPARATOR);
}

static inline void
check_coords(struct coords coords)
{
	/* Check coordinate validity before drawing, ensure at least one row, column */

	if (coords.r1 > coords.rN)
		fatal("row coordinates invalid");

	if (coords.c1 > coords.cN)
		fatal("column coordinates invalid");
}

static inline unsigned int
nick_col(char *nick)
{
	unsigned int colour = 0;

	while (*nick)
		colour += *nick++;

	return nick_colours[colour % sizeof(nick_colours) / sizeof(nick_colours[0])];
}

static int
_draw_fmt(char *buff, size_t *offset, size_t *buff_n, size_t *text_n, int txt, const char *fmt, ...)
{
	/* Write formatted text to a buffer for purposes of preparing an object to be drawn
	 * to the terminal.
	 *
	 * Calls to this function should distinguish between formatting and printed text
	 * with the txt flag.
	 * */

	int ret;
	size_t _ret;
	va_list ap;

	/* Man vsnprintf: "... the number of characters (excluding the terminating null byte) which
	 * would have been writting to the final string if enough space had been avilable. Thus,
	 * a return value of `size` or more means that the output was truncated" */
	va_start(ap, fmt);
	ret = vsnprintf(buff + *offset, *buff_n, fmt, ap);
	va_end(ap);

	/* In any case of failure, truncate the buffer where printing would have began */
	if (ret < 0)
		return (*buff = 0);

	_ret = (size_t)ret;

	/* If printing formatting escape sequences and insufficient room, truncate any partial
	 * sequence that would be printed */
	if (!txt && _ret >= *buff_n)
		return (*buff = 0);

	if (txt) {

		/* If printing text and insufficient text columns available, truncate after any printable
		 * characters */
		if (_ret >= *text_n)
			return (*(buff + *text_n) = 0);

		/* Either calls to this function were erroneously flagged or insufficient room was
		 * allocated for all the formatting */
		if (_ret >= *buff_n)
			fatal("text columns available but buffer is full");

		*text_n -= _ret;
	}

	*offset += _ret;
	*buff_n -= _ret;

	return 1;
}

void
split_buffer_cols(
	struct buffer_line *line,
	unsigned int *head_w,
	unsigned int *text_w,
	unsigned int cols,
	unsigned int pad)
{
	unsigned int _head_w = sizeof(" HH:MM   "VERTICAL_SEPARATOR" ");

	if (BUFFER_PADDING)
		_head_w += pad;
	else
		_head_w += line->from_len;

	/* If header won't fit, split in half */
	if (_head_w >= cols)
		_head_w = cols / 2;

	//TODO: why?
	_head_w -= 1;

	if (head_w)
		*head_w = _head_w;
	if (text_w)
		*text_w = cols - _head_w + 1;
}
