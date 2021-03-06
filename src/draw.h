#ifndef DRAW_H
#define DRAW_H

/* Draw all components */
void draw_all(void);
void draw(void);

/* Draw component, e.g. draw_buffer(); */
#define DRAW_BITS \
	X(buffer) \
	X(input)  \
	X(nav)    \
	X(status)

/* Function prototypes for setting draw bits */
#define X(bit) void draw_##bit(void);
DRAW_BITS
#undef X

void split_buffer_cols(struct buffer_line*, unsigned int*, unsigned int*, unsigned int, unsigned int);

#endif
