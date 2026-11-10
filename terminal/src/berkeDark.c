/*
 * berkeDark for the terminal: a colour interpreter.
 *
 * A program says "italic" or "strikethrough" with a style mark that carries no
 * colour, and the terminal has no setting for it. berkeDark runs the program in a
 * pseudo-terminal of its own, reads what it prints, and adds the colour right after
 * such a mark. Every byte the program prints reaches the terminal unchanged and in
 * order; keys, mouse, window size and exit status pass through.
 *
 * On a full-screen program it also keeps a small map of the screen, which rows are
 * used and which carry the user's band colour, and at the end of each frame draws a
 * box around those rows on top of what the program drew. The same map knows which rows
 * are code rows of a diff, where the program's syntax colours get berkeDark's, and where
 * Claude Code's prompt mark starts a row, which it writes again in white, solid on the
 * rows the user has sent. Numbers standing alone in a reply are written again in a colour
 * of their own when their word ends.
 *
 * Claude Code draws most of a reply's text in one colour: a row that is bold from end to end,
 * a label, a list's mark, a task's box, an address, a table's lines and a rule all arrive as
 * plain text. A second map keeps each cell's character, so at the end of each frame berkeDark
 * reads the rows of a reply, finds these things and writes their cells again in their colours;
 * a cell the program writes later is the program's again until the next frame ends.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "colours.h"

enum { BLOCK_SIZE = 65536, MAX_ROWS = 256, MAX_COLUMNS = 512 };

/*
 * CELL_PLAIN: a character in the program's default look (bold or not), which berkeDark may write again.
 * CELL_BOLD: a bold character of any colour. CELL_REPLY: the mark that starts Claude Code's reply text.
 */
enum { CELL_USED = 1, CELL_BAND = 2, CELL_POINTER = 4, CELL_TOOL = 8, CELL_PLAIN = 16, CELL_BOLD = 32, CELL_REPLY = 64, CELL_INPUT = 128 };

/* The second map, one word a cell: the character, how berkeDark has written the cell again, and two marks. */
enum {
    GLYPH_CODE = 0x1fffff,             /* the character's code point; 0: none known */
    PAINT_SHIFT = 21,
    GLYPH_PAINT = 0x3f << PAINT_SHIFT, /* a paint kind */
    GLYPH_JOINED = 1 << 27,            /* more code points follow the character: such a cell is never written again */
    GLYPH_TAIL = 1 << 28,              /* the second cell of a wide character */
    GLYPH_JOINER = 1 << 29,            /* the character ends with a joiner (U+200D) so far: a picture after it joins it */
    GLYPH_APOSTROPHE = 1 << 30,        /* on a diff's row: drawn as plain text, inside a "string" that an apostrophe began */
};

/*
 * The third map, one byte a cell: how the program drew the cell. With it berkeDark can write a coloured
 * cell again in a colour of its own, and later give it back as the program drew it.
 */
enum {
    LOOK_INDEX = 0x0f,  /* the palette colour, 0 to 15, when LOOK_COLOUR is set */
    LOOK_COLOUR = 0x10, /* the program chose a palette colour */
    LOOK_DIM = 0x20,
    LOOK_ITALIC = 0x40,
    LOOK_OTHER = 0x80,  /* a look berkeDark does not write again: a background, an underline, a colour by value, a tint */
};

/* How a cell stands on the terminal after berkeDark wrote it again. PAINT_NONE: as the program drew it. */
enum {
    PAINT_NONE,
    PAINT_NUMBER,
    PAINT_DATE,
    PAINT_HEADING_LINE,
    PAINT_LABEL,
    PAINT_LINK_URL,
    PAINT_LINK_BARE,
    PAINT_LIST_MARK,
    PAINT_TASK_OPEN,
    PAINT_TASK_DONE,
    PAINT_HIDDEN, /* a blank in place of the character */
    PAINT_TABLE_BORDER,
    PAINT_TABLE_HEADER,
    PAINT_RULE,
    PAINT_CODE_INLINE,
    PAINT_PATH,
    PAINT_CODE_KEYWORD,
    PAINT_CODE_CONSTANT,
    PAINT_CODE_CLASS,
    PAINT_CODE_FUNCTION,
    PAINT_CODE_TYPE,
    PAINT_CODE_CALL,
    PAINT_CODE_KEY,
    PAINT_CODE_STRING,
    PAINT_CODE_NUMBER,
    PAINT_CODE_COMMENT,
    PAINT_CODE_DECORATOR,
    PAINT_CODE_MACRO,
    PAINT_CODE_PUNCTUATION,
    PAINT_COMMAND_NAME,
    PAINT_COMMAND_OPTION,
    PAINT_COMMAND_VARIABLE,
    PAINT_COMMAND_OPERATOR,
    PAINT_COMMAND_TEXT,
    PAINT_SUGGESTION,
};

enum { NO_EDGE, TOP_EDGE, BOTTOM_EDGE, STALE_EDGE };

enum state { GROUND, ESCAPE, ESCAPE_INTERMEDIATE, CONTROL_SEQUENCE, STRING };

struct style {
    bool bold, dim, italic, underline, strike, reverse;
    bool program_colour; /* the program chose the text colour itself */
    bool background;     /* the program chose a background */
    bool band;           /* that background is the user's band */
    const char *tint;    /* the colour berkeDark has on the terminal, if any */
    int foreground;      /* the program's colour mark: 30-37, 90-97, 38 (extended), 0 none */
    char extended[40];   /* the arguments after 38, to write the mark again */
    const char *syntax;  /* the syntax colour berkeDark has on the terminal, if any */
    int ground;            /* the program's background when it is one of a diff's greys (235, 237, 238), else 0 */
    const char *ground_on; /* the ground berkeDark has on the terminal in its place, if any */
    bool is_apostrophe;    /* the character being printed belongs to a "string" that an apostrophe began */
    bool is_colour_start;  /* the program has set its text colour and printed nothing since */
};

struct screen {
    bool is_alternate;
    bool is_claude;  /* the full-screen program is Claude Code: its marks and diff colours apply */
    bool has_saved_cursor, is_insert, is_origin; /* the program's states in which numbers are left alone */
    bool is_no_wrap; /* the program turned automatic wrapping off; a terminal mode, so a new map keeps it */
    bool has_band_changed;
    bool is_dirty;     /* the map changed since the reply text was last settled */
    int rows, columns; /* 0 rows: no map is kept */
    int row, column, saved_row, saved_column;
    int top, bottom; /* scrolling region */
    unsigned code_point;
    int bytes_left;
    unsigned char edges[MAX_ROWS];
    unsigned char cells[MAX_ROWS][MAX_COLUMNS];
    short code_from[MAX_ROWS]; /* a diff's code row: its first code column; -1 for other rows */
    char run_kind;             /* the gutter being printed: 'd' dim number, 'm' + or - mark, 0 none */
    int run_row, run_column, run_length;
    char run_text[16];
    int word_row, word_column, word_length; /* the run of characters being printed; length 0: none */
    unsigned char text[MAX_ROWS][MAX_COLUMNS]; /* 0: blank or a separator; 0x80: any non-ASCII character */
    unsigned glyphs[MAX_ROWS][MAX_COLUMNS];    /* the second map: GLYPH_CODE, GLYPH_PAINT and the marks */
    unsigned char looks[MAX_ROWS][MAX_COLUMNS]; /* the third map: how the program drew the cell */
};

struct interpreter {
    enum state state;
    bool bell_ends_string;
    bool is_plain;   /* the sequence so far holds only digits, ';' and ':' */
    bool is_private; /* it started with '?' */
    size_t length;
    char parameters[256];
    struct style style, saved_style;
    struct screen screen;
};

static struct interpreter interpreter;

static int program_master = -1; /* the pseudo-terminal; -1 under --filter, where the stream is taken as Claude Code's */

static void end_gutter_run(struct screen *sc);
static void end_word(struct screen *sc);
static bool is_input_row(const struct screen *sc, int row);
static void put_program_colour(const struct style *s);

static unsigned char output[BLOCK_SIZE];
static size_t output_length;
static bool terminal_gone;

static volatile sig_atomic_t child_exited, window_resized, quit_signal;

static void write_all(int fd, const unsigned char *data, size_t size)
{
    while (size > 0 && !terminal_gone) {
        ssize_t written = write(fd, data, size);
        if (written >= 0) {
            data += written;
            size -= (size_t)written;
        } else if (errno == EAGAIN) {
            struct pollfd ready = {.fd = fd, .events = POLLOUT};
            poll(&ready, 1, -1);
        } else if (errno != EINTR) {
            terminal_gone = true;
        }
    }
}

static void flush_output(void)
{
    write_all(STDOUT_FILENO, output, output_length);
    output_length = 0;
}

static void put(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t i = 0; i < size; ++i) {
        if (output_length == sizeof output)
            flush_output();
        output[output_length++] = bytes[i];
    }
}

/*
 * A top heading arrives bold, italic and underlined at once. Dim text is a program's
 * quiet interface text and keeps its look.
 */
static const char *wanted_tint(const struct style *s)
{
    if (s->program_colour || s->dim)
        return NULL;
    if (s->bold && s->italic && s->underline)
        return HEADING_COLOUR;
    if (s->bold && s->italic)
        return BOLD_ITALIC_COLOUR;
    if (s->italic)
        return ITALIC_COLOUR;
    if (s->strike)
        return STRIKE_COLOUR;
    return NULL;
}

static void settle_tint(struct style *s, bool colour_changed)
{
    if (colour_changed)
        s->tint = NULL; /* the program's own colour mark replaced ours */

    const char *wanted = wanted_tint(s);

    if (wanted != NULL && wanted != s->tint) {
        put("\033[38;2;", 7);
        put(wanted, strlen(wanted));
        put("m", 1);
        s->syntax = NULL; /* the tint replaced it on the terminal */
    } else if (wanted == NULL && s->tint != NULL) {
        put("\033[39m", 5);
        s->syntax = NULL;
    }
    s->tint = wanted;
}

/* Moves past the arguments of 38, 48 or 58 written with semicolons. */
static const char *skip_colour_arguments(const char *p)
{
    if (*p != ';')
        return p;

    int arguments = p[1] == '5' ? 2 : p[1] == '2' ? 4 : 1;

    while (arguments-- > 0 && *p == ';') {
        ++p;
        while (*p != '\0' && *p != ';')
            ++p;
    }
    return p;
}

static void apply_style_sequence(struct interpreter *in)
{
    struct style *s = &in->style;
    const char *p = in->parameters;
    bool colour_changed = false;

    for (;;) {
        const char *start = p;
        unsigned code = 0;
        while (isdigit((unsigned char)*p))
            code = code * 10 + (unsigned)(*p++ - '0');

        bool has_subparameter = *p == ':';
        bool is_underline_off = has_subparameter && p[1] == '0';
        while (*p != '\0' && *p != ';')
            ++p;

        switch (code) {
        case 0:
            end_gutter_run(&in->screen);
            *s = (struct style){0};
            colour_changed = true;
            break;
        case 1: s->bold = true; break;
        case 2: s->dim = true; break;
        case 3: s->italic = true; break;
        case 4: s->underline = !is_underline_off; break;
        case 7: s->reverse = true; break;
        case 9: s->strike = true; break;
        case 21: s->underline = true; break;
        case 22: end_gutter_run(&in->screen); s->bold = s->dim = false; break;
        case 23: s->italic = false; break;
        case 24: s->underline = false; break;
        case 27: s->reverse = false; break;
        case 29: s->strike = false; break;
        case 30 ... 37:
        case 90 ... 97:
            s->program_colour = true;
            s->foreground = (int)code;
            s->is_colour_start = true;
            colour_changed = true;
            break;
        case 38: {
            const char *arguments = p;
            s->program_colour = true;
            s->foreground = 38;
            colour_changed = true;
            if (!has_subparameter)
                p = skip_colour_arguments(p);
            size_t length = (size_t)(p - arguments);
            if (length >= sizeof s->extended)
                length = 0; /* too long to write again; 38 alone is never written back */
            memcpy(s->extended, arguments, length);
            s->extended[length] = '\0';
            break;
        }
        case 39:
            s->program_colour = false;
            s->foreground = 0;
            colour_changed = true;
            break;
        case 40 ... 47:
        case 100 ... 107:
            s->background = true;
            s->band = false;
            s->ground = 0;
            s->ground_on = NULL;
            break;
        case 48:
            s->background = true;
            s->band = strncmp(start, BAND_BACKGROUND, strlen(BAND_BACKGROUND)) == 0;
            s->ground = strncmp(start, "48;5;23", 7) == 0 && strchr("578", start[7]) != NULL && start[7] != '\0' &&
                                (start[8] == ';' || start[8] == '\0')
                            ? 230 + (start[7] - '0')
                            : 0;
            s->ground_on = NULL; /* the program's own mark reached the terminal after ours */
            if (!has_subparameter)
                p = skip_colour_arguments(p);
            break;
        case 49:
            s->background = s->band = false;
            s->ground = 0;
            s->ground_on = NULL;
            break;
        case 58:
            if (!has_subparameter)
                p = skip_colour_arguments(p);
            break;
        default:
            break;
        }

        if (*p != ';')
            break;
        ++p;
    }
    if (colour_changed)
        s->syntax = NULL; /* the program's own mark reached the terminal after ours */
    settle_tint(s, colour_changed);
}

/* The map of the screen. A write only ever changes the map, never the program's bytes. */

static bool has_map(const struct screen *sc)
{
    return sc->is_alternate && sc->rows > 0;
}

static void clear_map(struct screen *sc)
{
    memset(sc->cells, 0, sizeof sc->cells);
    memset(sc->text, 0, sizeof sc->text);
    memset(sc->glyphs, 0, sizeof sc->glyphs);
    memset(sc->looks, 0, sizeof sc->looks);
    sc->is_dirty = false;
    memset(sc->edges, NO_EDGE, sizeof sc->edges);
    memset(sc->code_from, 0xff, sizeof sc->code_from);
    sc->run_kind = 0;
    sc->run_length = 0;
    sc->word_length = 0;
    sc->has_saved_cursor = sc->is_insert = sc->is_origin = false;
    sc->row = sc->column = sc->saved_row = sc->saved_column = 0;
    sc->top = 0;
    sc->bottom = sc->rows - 1;
    sc->has_band_changed = false;
}

static void set_map_size(struct screen *sc, int rows, int columns)
{
    bool fits = rows > 0 && columns > 1 && rows <= MAX_ROWS && columns <= MAX_COLUMNS;

    sc->rows = fits ? rows : 0;
    sc->columns = fits ? columns : 0;
    clear_map(sc);
}

static void set_cell(struct screen *sc, int row, int column, unsigned char flags)
{
    unsigned char *cell = &sc->cells[row][column];

    if (sc->edges[row] != NO_EDGE)
        sc->edges[row] = STALE_EDGE;
    if (((*cell | flags) & CELL_BAND) != 0)
        sc->has_band_changed = true;
    *cell = flags;
    sc->text[row][column] = 0;
    sc->glyphs[row][column] = 0; /* what the program writes is no longer painted */
    sc->looks[row][column] = 0;
    sc->is_dirty = true;
}

static unsigned glyph_at(const struct screen *sc, int row, int column)
{
    return sc->glyphs[row][column] & GLYPH_CODE;
}

static int paint_at(const struct screen *sc, int row, int column)
{
    return (int)((sc->glyphs[row][column] & GLYPH_PAINT) >> PAINT_SHIFT);
}

static void set_paint(struct screen *sc, int row, int column, int kind)
{
    sc->glyphs[row][column] = (sc->glyphs[row][column] & ~(unsigned)GLYPH_PAINT) | (unsigned)kind << PAINT_SHIFT;
}

static unsigned char blank_flags(const struct style *s)
{
    return s->band ? CELL_BAND : s->background ? CELL_USED : 0;
}

static void clear_cells(struct interpreter *in, int row, int from, int to)
{
    struct screen *sc = &in->screen;

    for (int column = from; column < to && column < sc->columns; ++column)
        set_cell(sc, row, column, blank_flags(&in->style));
}

/* Moves rows up inside top..bottom, or down when count is negative. */
static void scroll_rows(struct screen *sc, int top, int bottom, int count)
{
    int height = bottom - top + 1;

    if (height <= 0)
        return;

    int amount = abs(count) < height ? abs(count) : height;
    size_t kept = (size_t)(height - amount);

    /* The terminal moves what berkeDark painted along with the rows, so the second map moves as it is. */
    if (count > 0) {
        memmove(sc->cells[top], sc->cells[top + amount], kept * sizeof sc->cells[0]);
        memmove(sc->text[top], sc->text[top + amount], kept * sizeof sc->text[0]);
        memmove(sc->glyphs[top], sc->glyphs[top + amount], kept * sizeof sc->glyphs[0]);
        memmove(sc->looks[top], sc->looks[top + amount], kept * sizeof sc->looks[0]);
        memmove(&sc->edges[top], &sc->edges[top + amount], kept);
        memmove(&sc->code_from[top], &sc->code_from[top + amount], kept * sizeof sc->code_from[0]);
        memset(sc->cells[bottom - amount + 1], 0, (size_t)amount * sizeof sc->cells[0]);
        memset(sc->text[bottom - amount + 1], 0, (size_t)amount * sizeof sc->text[0]);
        memset(sc->glyphs[bottom - amount + 1], 0, (size_t)amount * sizeof sc->glyphs[0]);
        memset(sc->looks[bottom - amount + 1], 0, (size_t)amount * sizeof sc->looks[0]);
        memset(&sc->edges[bottom - amount + 1], NO_EDGE, (size_t)amount);
        memset(&sc->code_from[bottom - amount + 1], 0xff, (size_t)amount * sizeof sc->code_from[0]);
    } else {
        memmove(sc->cells[top + amount], sc->cells[top], kept * sizeof sc->cells[0]);
        memmove(sc->text[top + amount], sc->text[top], kept * sizeof sc->text[0]);
        memmove(sc->glyphs[top + amount], sc->glyphs[top], kept * sizeof sc->glyphs[0]);
        memmove(sc->looks[top + amount], sc->looks[top], kept * sizeof sc->looks[0]);
        memmove(&sc->edges[top + amount], &sc->edges[top], kept);
        memmove(&sc->code_from[top + amount], &sc->code_from[top], kept * sizeof sc->code_from[0]);
        memset(sc->cells[top], 0, (size_t)amount * sizeof sc->cells[0]);
        memset(sc->text[top], 0, (size_t)amount * sizeof sc->text[0]);
        memset(sc->glyphs[top], 0, (size_t)amount * sizeof sc->glyphs[0]);
        memset(sc->looks[top], 0, (size_t)amount * sizeof sc->looks[0]);
        memset(&sc->edges[top], NO_EDGE, (size_t)amount);
        memset(&sc->code_from[top], 0xff, (size_t)amount * sizeof sc->code_from[0]);
    }
    sc->run_kind = 0;
    sc->run_length = 0;
    sc->is_dirty = true;
}

static void line_feed(struct screen *sc)
{
    if (sc->row == sc->bottom)
        scroll_rows(sc, sc->top, sc->bottom, 1);
    else if (sc->row < sc->rows - 1)
        ++sc->row;
}

static void reverse_index(struct screen *sc)
{
    if (sc->row == sc->top)
        scroll_rows(sc, sc->top, sc->bottom, -1);
    else if (sc->row > 0)
        --sc->row;
}

/*
 * The gutter of a diff row. Claude Code writes a row's line number dim (" 12 "), or with the
 * + or - mark in its added or removed colour (" 12 +"); a wrapped row repeats the gutter without
 * the number ("   +", or dim spaces), so that form counts only right below a code row. Code
 * starts right after the gutter. A row whose gutter is written over with anything else is
 * not a code row any more.
 */
static bool is_code_row(const struct screen *sc, int row)
{
    return row >= 0 && row < sc->rows && sc->code_from[row] >= 0;
}

/*
 * A row of the input box never becomes a code row: its dim hint may start with a digit, which looks like
 * a diff's line number and would leave the first typed character without the input colour.
 */
static bool may_start_code(const struct screen *sc)
{
    if (is_input_row(sc, sc->run_row))
        return false;
    return sc->code_from[sc->run_row] < 0 || sc->run_column < sc->code_from[sc->run_row];
}

static void end_gutter_run(struct screen *sc)
{
    if (sc->run_kind == 'd' && sc->run_length > 0 && sc->run_row < sc->rows && may_start_code(sc)) {
        bool has_number = false;
        for (int i = 0; i < sc->run_length; ++i)
            has_number |= isdigit((unsigned char)sc->run_text[i]) != 0;
        if (has_number || is_code_row(sc, sc->run_row - 1))
            sc->code_from[sc->run_row] = (short)(sc->run_column + sc->run_length);
    }
    sc->run_kind = 0;
    sc->run_length = 0;
}

static void check_mark_run(struct screen *sc)
{
    const char *t = sc->run_text;
    int n = sc->run_length, i = 0;

    if (n < 2 || (t[n - 1] != '+' && t[n - 1] != '-') || !may_start_code(sc))
        return;
    while (i < n - 1 && t[i] == ' ')
        ++i;
    int digits = i;
    while (i < n - 1 && isdigit((unsigned char)t[i]))
        ++i;
    bool has_number = i > digits;
    if (has_number ? !(i == n - 2 && t[i] == ' ') : i != n - 1)
        return;
    if (has_number || is_code_row(sc, sc->run_row - 1))
        sc->code_from[sc->run_row] = (short)(sc->run_column + n);
}

static void follow_gutter(struct interpreter *in, unsigned code_point)
{
    struct screen *sc = &in->screen;
    const struct style *s = &in->style;
    char kind = 0;

    if (code_point < 0x80) {
        bool gutter_character = code_point == ' ' || isdigit((int)code_point);
        if (s->dim && gutter_character)
            kind = 'd';
        else if (!s->dim && (s->foreground == 91 || s->foreground == 92) &&
                 (gutter_character || code_point == '+' || code_point == '-'))
            kind = 'm';
    }
    bool continues = kind != 0 && kind == sc->run_kind && sc->run_row == sc->row &&
                     sc->run_column + sc->run_length == sc->column &&
                     sc->run_length < (int)sizeof sc->run_text;
    if (!continues) {
        if (sc->run_kind == 'd' && kind == 0 && s->dim) {
            /* Dim text goes on right after the dim number: a quiet line of text, not a diff's line number. */
            sc->run_kind = 0;
            sc->run_length = 0;
        } else {
            end_gutter_run(sc);
        }
        if (kind != 0) {
            sc->run_kind = kind;
            sc->run_row = sc->row;
            sc->run_column = sc->column;
        }
    }
    if (kind != 0) {
        sc->run_text[sc->run_length++] = (char)code_point;
        if (kind == 'm')
            check_mark_run(sc);
    } else if (sc->code_from[sc->row] >= 0 && sc->column < sc->code_from[sc->row]) {
        sc->code_from[sc->row] = -1;
    }
}

/*
 * Claude Code's prompt mark at the start of a row, in white (the author, 2026-11-08). A sent row carries
 * the user's band and gets SENT_MARK; the input row has no background, the program's default colour, or
 * grey while Claude works, and gets INPUT_MARK. The two differ in the program's own drawing too, so it
 * never skips the cell when one takes the other's place. The cursor has just passed the mark: one cell
 * back, the mark again, and the colour on the terminal back. The grey of the input row while Claude works
 * stays grey (the author's choice). Any other mark at the start of a row stays as it is.
 */
static void redraw_prompt_mark(const struct style *s, bool in_claude)
{
    static const char white[] = "\033[38;2;" PROMPT_MARK_COLOUR "m";
    bool is_sent = s->band;
    bool is_input = !s->band && (s->foreground == 0 || s->foreground == 90);

    if (!in_claude || s->dim || (!is_sent && !is_input))
        return;

    const char *mark = is_sent ? SENT_MARK : INPUT_MARK;
    bool is_white = is_sent || s->foreground == 0;

    if (!is_white && strcmp(mark, "\u276f") == 0)
        return;
    put("\033[D", 3);
    if (is_white)
        put(white, sizeof white - 1);
    put(mark, strlen(mark));
    if (!is_white)
        return;
    if (s->tint != NULL) {
        put("\033[38;2;", 7);
        put(s->tint, strlen(s->tint));
        put("m", 1);
    } else {
        put_program_colour(s);
    }
}

/* A rule of Claude Code's input box: a row drawn from edge to edge with line characters. */
static bool is_rule_row(const struct screen *sc, int row)
{
    for (int column = 0; column < sc->columns; ++column)
        if (sc->text[row][column] != 0x80)
            return false;
    return true;
}

/* The mark of the input box: the program's default colour, or grey while Claude works, right under the box's rule. */
static bool is_input_mark(const struct interpreter *in)
{
    const struct style *s = &in->style;
    const struct screen *sc = &in->screen;

    if (!sc->is_claude || s->band || s->background || s->dim || (s->foreground != 0 && s->foreground != 90))
        return false;
    return sc->row > 0 && is_rule_row(sc, sc->row - 1);
}

/* A row of the input box: the row of its mark, or a row under it down to the box's lower rule. */
static bool is_input_row(const struct screen *sc, int row)
{
    for (int r = row; r >= 0 && r > row - 64; --r) {
        if ((sc->cells[r][0] & CELL_INPUT) != 0)
            return true;
        if ((sc->cells[r][0] & CELL_USED) != 0)
            return false;
    }
    return false;
}

static bool is_separator(unsigned code_point)
{
    return code_point == ' ' || code_point == 0xa0 || code_point == 0x2502;
}

/*
 * Follows the run being printed; a separator or a gap ends it (and so does any escape, in interpret).
 * The character is in the map already, at the given column: the terminal has drawn it, and a word that ends
 * here is read from the map, which must not show what stood in that cell before.
 */
static void follow_word(struct screen *sc, unsigned code_point, int width, int column)
{
    if (is_separator(code_point)) {
        end_word(sc);
        return;
    }
    if (sc->word_length > 0 && (sc->word_row != sc->row || sc->word_column + sc->word_length != column))
        end_word(sc);
    if (sc->word_length == 0) {
        sc->word_row = sc->row;
        sc->word_column = column;
    }
    sc->word_length += width;
}

/* Roughly the code points that are pictures: the ones a joiner (U+200D) ties to the picture before it. */
static bool is_pictograph(unsigned code_point)
{
    return (code_point >= 0x1f000 && code_point <= 0x1faff) || (code_point >= 0x2600 && code_point <= 0x27bf) ||
           (code_point >= 0x2b00 && code_point <= 0x2bff) || (code_point >= 0x2190 && code_point <= 0x21ff) ||
           (code_point >= 0x2300 && code_point <= 0x23ff);
}

/*
 * A code point that goes on with the character before it: a mark without width (an accent, a selector, a
 * joiner), an emoji's skin tone, or a picture after a joiner. foot keeps such a character in the cells of
 * its first code point, two at most, and the emoji selector (U+FE0F) makes them two (measured on foot 1.28,
 * 2026-11-10); the map follows that, or every cell after it on the row would be one off. The map keeps one
 * code point a cell, so the character could not be written again whole: its cells are marked and left alone.
 * Returns whether the code point was taken.
 */
static bool join_character(struct screen *sc, unsigned code_point, int width)
{
    bool is_tone = code_point >= 0x1f3fb && code_point <= 0x1f3ff;
    int last = sc->column - 1;

    if (sc->row >= sc->rows || last < 0 || last >= sc->columns)
        return width == 0; /* nothing before it; a mark without width takes no cell */

    int head = (sc->glyphs[sc->row][last] & GLYPH_TAIL) != 0 && last > 0 ? last - 1 : last;
    unsigned *cell = &sc->glyphs[sc->row][head];
    unsigned char flags = sc->cells[sc->row][head];

    if (width != 0 && !is_tone && !((*cell & GLYPH_JOINER) != 0 && is_pictograph(code_point)))
        return false;
    if ((flags & CELL_USED) == 0)
        return width == 0; /* a blank before it */

    int cells_now = last - head + 1;
    int cells_wanted = cells_now + (code_point == 0xfe0f ? 2 : width > 0 ? width : 0);

    *cell = ((*cell | GLYPH_JOINED) & ~(unsigned)GLYPH_JOINER) | (code_point == 0x200d ? (unsigned)GLYPH_JOINER : 0);
    if (head != last)
        sc->glyphs[sc->row][last] |= GLYPH_JOINED;
    if (cells_wanted > 1 && cells_now == 1 && sc->column < sc->columns) {
        /* The character grows into a second cell. */
        if (sc->word_length > 0 && sc->word_row == sc->row && sc->word_column + sc->word_length == sc->column)
            ++sc->word_length;
        set_cell(sc, sc->row, sc->column, flags & (CELL_USED | CELL_BAND | CELL_PLAIN | CELL_BOLD));
        sc->glyphs[sc->row][sc->column] = (unsigned)GLYPH_TAIL | GLYPH_JOINED;
        sc->text[sc->row][sc->column] = sc->text[sc->row][head] != 0 ? 0x80 : 0;
        ++sc->column;
    }
    sc->is_dirty = true;
    return true;
}

static void print_code_point(struct interpreter *in, unsigned code_point)
{
    struct screen *sc = &in->screen;
    int width = code_point < 0x80 ? 1 : wcwidth((wchar_t)code_point);

    if (code_point >= 0x80 && join_character(sc, code_point, width))
        return;
    if (width == 0)
        return;

    /* Only a character the terminal surely draws as it was written is kept for writing again. */
    bool is_known = width > 0 && code_point <= 0x10ffff && (code_point < 0xd800 || code_point > 0xdfff);

    if (width < 0)
        width = 1;
    if (sc->column + width > sc->columns) {
        sc->word_length = 0;
        if (sc->is_no_wrap) {
            sc->column = sc->columns - width; /* the end of the row is written again */
        } else {
            sc->column = 0;
            line_feed(sc);
        }
    }
    follow_gutter(in, code_point);

    unsigned char flags = blank_flags(&in->style);
    int first_column = sc->column;

    if (code_point != ' ')
        flags |= CELL_USED;
    if (code_point == 0x23bf)
        flags |= CELL_TOOL;
    bool is_prompt_mark = code_point == 0x276f && sc->column == 0;
    if (is_prompt_mark && !in->style.band)
        flags |= CELL_POINTER;
    if (is_prompt_mark && is_input_mark(in))
        flags |= CELL_INPUT;

    const struct style *s = &in->style;
    bool is_word_character = !is_separator(code_point);

    if (code_point != ' ' && s->foreground == 0 && !s->background && !s->dim && !s->italic && !s->underline &&
        !s->strike && !s->reverse && s->tint == NULL)
        flags |= CELL_PLAIN;
    if (code_point != ' ' && s->bold)
        flags |= CELL_BOLD;
    if (code_point == 0x25cf && sc->column == 0 && s->foreground == 38 && strcmp(s->extended, REPLY_MARK_FOREGROUND) == 0)
        flags |= CELL_REPLY;

    unsigned char look = 0;

    if (s->background || s->underline || s->strike || s->reverse || s->foreground == 38 || s->tint != NULL) {
        look = LOOK_OTHER;
    } else {
        if (s->foreground != 0)
            look |= (unsigned char)(LOOK_COLOUR | (s->foreground < 90 ? s->foreground - 30 : s->foreground - 82));
        if (s->dim)
            look |= LOOK_DIM;
        if (s->italic)
            look |= LOOK_ITALIC;
    }
    for (int i = 0; i < width; ++i) {
        set_cell(sc, sc->row, sc->column, flags);
        sc->looks[sc->row][sc->column] = look;
        sc->glyphs[sc->row][sc->column] = (i > 0 ? (unsigned)GLYPH_TAIL : is_known ? code_point : 0) |
                                          (s->is_apostrophe ? (unsigned)GLYPH_APOSTROPHE : 0);
        sc->text[sc->row][sc->column++] = !is_word_character ? 0 : code_point < 0x80 ? (unsigned char)code_point : 0x80;
    }
    follow_word(sc, code_point, width, first_column);
    if (is_prompt_mark)
        redraw_prompt_mark(&in->style, sc->is_claude);
}

static void read_text(struct interpreter *in, unsigned char c)
{
    struct screen *sc = &in->screen;

    if (c < 0x80) {
        sc->bytes_left = 0;
        print_code_point(in, c);
    } else if ((c & 0xc0) == 0x80) {
        if (sc->bytes_left > 0) {
            sc->code_point = sc->code_point << 6 | (c & 0x3f);
            if (--sc->bytes_left == 0)
                print_code_point(in, sc->code_point);
        }
    } else {
        sc->bytes_left = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        sc->code_point = c & (0x3fu >> sc->bytes_left);
    }
}

static void read_control_character(struct screen *sc, unsigned char c)
{
    end_gutter_run(sc);
    switch (c) {
    case '\r': sc->column = 0; break;
    case '\n':
    case '\v':
    case '\f': line_feed(sc); break;
    case '\b':
        if (sc->column > 0)
            --sc->column;
        break;
    case '\t':
        sc->column = (sc->column / 8 + 1) * 8;
        if (sc->column >= sc->columns)
            sc->column = sc->columns - 1;
        break;
    default: break;
    }
}

static void keep_cursor_on_screen(struct screen *sc)
{
    if (sc->row < 0)
        sc->row = 0;
    if (sc->row >= sc->rows)
        sc->row = sc->rows - 1;
    if (sc->column < 0)
        sc->column = 0;
    if (sc->column >= sc->columns)
        sc->column = sc->columns - 1;
}

static void erase_in_display(struct interpreter *in, int mode)
{
    struct screen *sc = &in->screen;

    for (int row = 0; row < sc->rows; ++row) {
        bool erased = mode == 2 || mode == 3 || (mode == 0 && row > sc->row) || (mode == 1 && row < sc->row) ||
                      (row == sc->row && (mode == 1 || sc->column <= sc->code_from[row]));
        if (erased)
            sc->code_from[row] = -1;
    }

    if (mode == 0) {
        clear_cells(in, sc->row, sc->column, sc->columns);
        for (int row = sc->row + 1; row < sc->rows; ++row)
            clear_cells(in, row, 0, sc->columns);
    } else if (mode == 1) {
        clear_cells(in, sc->row, 0, sc->column + 1);
        for (int row = 0; row < sc->row; ++row)
            clear_cells(in, row, 0, sc->columns);
    } else if (mode == 2) {
        for (int row = 0; row < sc->rows; ++row)
            clear_cells(in, row, 0, sc->columns);
    }
}

static void shift_cells(struct interpreter *in, int count)
{
    struct screen *sc = &in->screen;
    unsigned char *line = sc->cells[sc->row], *text = sc->text[sc->row], *looks = sc->looks[sc->row];
    unsigned *glyphs = sc->glyphs[sc->row];
    int room = sc->columns - sc->column;
    int amount = abs(count) < room ? abs(count) : room;

    if (count > 0) {
        memmove(line + sc->column + amount, line + sc->column, (size_t)(room - amount));
        memmove(text + sc->column + amount, text + sc->column, (size_t)(room - amount));
        memmove(glyphs + sc->column + amount, glyphs + sc->column, (size_t)(room - amount) * sizeof *glyphs);
        memmove(looks + sc->column + amount, looks + sc->column, (size_t)(room - amount));
        clear_cells(in, sc->row, sc->column, sc->column + amount);
    } else {
        memmove(line + sc->column, line + sc->column + amount, (size_t)(room - amount));
        memmove(text + sc->column, text + sc->column + amount, (size_t)(room - amount));
        memmove(glyphs + sc->column, glyphs + sc->column + amount, (size_t)(room - amount) * sizeof *glyphs);
        memmove(looks + sc->column, looks + sc->column + amount, (size_t)(room - amount));
        clear_cells(in, sc->row, sc->columns - amount, sc->columns);
    }
    sc->is_dirty = true;
}

/* A number in a sequence. A program may print any length of digits; a larger one than this means nothing on a screen. */
static int read_parameter(const char *p)
{
    enum { LARGEST_PARAMETER = 65535 };
    int value = 0;

    while (isdigit((unsigned char)*p) && value < LARGEST_PARAMETER)
        value = value * 10 + (*p++ - '0');
    return value < LARGEST_PARAMETER ? value : LARGEST_PARAMETER;
}

static void follow_cursor_sequence(struct interpreter *in, unsigned char final)
{
    struct screen *sc = &in->screen;
    int values[2] = {0, 0}, count = 0;

    for (const char *p = in->parameters; count < 2; ++count) {
        values[count] = read_parameter(p);
        p = strchr(p, ';');
        if (p == NULL)
            break;
        ++p;
    }

    int first = values[0], amount = first > 0 ? first : 1;

    end_gutter_run(sc);
    switch (final) {
    case 'A': sc->row -= amount; break;
    case 'B': sc->row += amount; break;
    case 'C': sc->column += amount; break;
    case 'D': sc->column -= amount; break;
    case 'E': sc->row += amount; sc->column = 0; break;
    case 'F': sc->row -= amount; sc->column = 0; break;
    case 'G':
    case '`': sc->column = amount - 1; break;
    case 'd': sc->row = amount - 1 + (sc->is_origin ? sc->top : 0); break;
    case 'H':
    case 'f':
        sc->row = amount - 1 + (sc->is_origin ? sc->top : 0);
        sc->column = (values[1] > 0 ? values[1] : 1) - 1;
        break;
    case 'J': erase_in_display(in, first); break;
    case 'K':
        keep_cursor_on_screen(sc);
        if (first != 0 || sc->column <= sc->code_from[sc->row])
            sc->code_from[sc->row] = -1;
        if (first == 0)
            clear_cells(in, sc->row, sc->column, sc->columns);
        else if (first == 1)
            clear_cells(in, sc->row, 0, sc->column + 1);
        else
            clear_cells(in, sc->row, 0, sc->columns);
        return;
    case 'X':
        keep_cursor_on_screen(sc);
        if (sc->column < sc->code_from[sc->row])
            sc->code_from[sc->row] = -1;
        clear_cells(in, sc->row, sc->column, sc->column + amount);
        return;
    case '@': keep_cursor_on_screen(sc); sc->code_from[sc->row] = -1; shift_cells(in, amount); return;
    case 'P': keep_cursor_on_screen(sc); sc->code_from[sc->row] = -1; shift_cells(in, -amount); return;
    case 'L':
    case 'M':
        /* Measured on foot: nothing happens outside the region, a pending wrap stays; inside it the cursor goes to the start of its row. */
        if (sc->row < sc->top || sc->row > sc->bottom)
            return;
        scroll_rows(sc, sc->row, sc->bottom, final == 'L' ? -amount : amount);
        sc->column = 0;
        break;
    case 'S': scroll_rows(sc, sc->top, sc->bottom, amount); break;
    case 'T': scroll_rows(sc, sc->top, sc->bottom, -amount); break;
    case 'r': {
        /* Measured on foot: a bottom past the screen is the last row; a region that is no region is ignored, a pending wrap stays. */
        int bottom = (values[1] > 0 && values[1] < sc->rows ? values[1] : sc->rows) - 1;

        if (amount - 1 >= bottom)
            return;
        sc->top = amount - 1;
        sc->bottom = bottom;
        sc->row = sc->is_origin ? sc->top : 0;
        sc->column = 0;
        break;
    }
    case 's':
        if (in->length != 0 && !isdigit((unsigned char)in->parameters[0]))
            return; /* a private sequence; measured on foot: plain CSI s and u save and restore, with or without a number */
        sc->saved_row = sc->row;
        sc->saved_column = sc->column;
        sc->has_saved_cursor = true;
        break;
    case 'u':
        if (in->length != 0 && !isdigit((unsigned char)in->parameters[0]))
            return;
        sc->row = sc->saved_row;
        sc->column = sc->saved_column;
        sc->has_saved_cursor = false;
        break;
    case 'b': /* the last character again, amount times */
        keep_cursor_on_screen(sc);
        for (int i = 0; i < amount && sc->column < sc->columns; ++i)
            set_cell(sc, sc->row, sc->column++, blank_flags(&in->style) | CELL_USED);
        break;
    case 'h':
    case 'l':
        if (first == 4)
            sc->is_insert = final == 'h';
        break;
    default: break;
    }
    if (sc->is_origin) { /* measured on foot: in origin mode every move stops at the region's edges */
        if (sc->row < sc->top)
            sc->row = sc->top;
        else if (sc->row > sc->bottom)
            sc->row = sc->bottom;
    }
    keep_cursor_on_screen(sc);
}

/* The box. It goes on top of the program's frame and never touches the map. */

static bool is_band_row(const struct screen *sc, int row)
{
    return (sc->cells[row][0] & sc->cells[row][sc->columns / 2] & CELL_BAND) != 0;
}

static bool is_blank_row(const struct screen *sc, int row)
{
    for (int column = 0; column < sc->columns; ++column)
        if (sc->cells[row][column] != 0)
            return false;
    return true;
}

static void put_text(const char *text)
{
    put(text, strlen(text));
}

static void put_move(int row, int column)
{
    char sequence[32];

    snprintf(sequence, sizeof sequence, "\033[%d;%dH", row + 1, column + 1);
    put_text(sequence);
}

/* Rows that belong to the interface: code, a tool's result or summary, the input row and its continuation. */
static bool is_interface_row(const struct screen *sc, int row)
{
    if (sc->code_from[row] >= 0)
        return true;
    for (int r = row; r >= 0; --r) {
        for (int column = 0; column < 6 && column < sc->columns; ++column)
            if ((sc->cells[r][column] & CELL_TOOL) != 0)
                return true;
        if ((sc->cells[r][0] & CELL_POINTER) != 0)
            return true;
        if ((sc->cells[r][0] & CELL_USED) != 0 || is_blank_row(sc, r))
            return false;
    }
    return false;
}

static int skip_digits(const char *t, int n, int i)
{
    while (i < n && isdigit((unsigned char)t[i]))
        ++i;
    return i;
}

/* Skips [.,]digits groups. */
static int skip_groups(const char *t, int n, int i)
{
    while (i + 1 < n && (t[i] == '.' || t[i] == ',') && isdigit((unsigned char)t[i + 1]))
        i = skip_digits(t, n, i + 1);
    return i;
}

static bool has_two_digits(const char *t, int n, int i)
{
    return i + 2 <= n && isdigit((unsigned char)t[i]) && isdigit((unsigned char)t[i + 1]) &&
           !(i + 2 < n && isdigit((unsigned char)t[i + 2]));
}

/* A number, range, percentage, date or time, with at most a bracket before it and a suffix or punctuation after. */
static bool find_number(const char *t, int n, int *from, int *to, bool *is_date)
{
    int i = n > 0 && (t[0] == '(' || t[0] == '[') ? 1 : 0;

    *from = i;
    *is_date = false;
    if (i < n && t[i] == '%')
        ++i;
    int start = i;
    i = skip_digits(t, n, i);
    int digits = i - start;

    if (digits == 0)
        return false;
    if (digits == 4 && i < n && t[i] == '-' && has_two_digits(t, n, i + 1) && i + 3 < n && t[i + 3] == '-' &&
        has_two_digits(t, n, i + 4)) {
        i += 6;
        *is_date = true;
    } else if (digits <= 2 && i < n && t[i] == ':' && has_two_digits(t, n, i + 1)) {
        i += 3;
        *is_date = true;
        if (i < n && t[i] == ':' && has_two_digits(t, n, i + 1))
            i += 3;
    } else {
        i = skip_groups(t, n, i);
        if (i + 1 < n && t[i] == '-' && isdigit((unsigned char)t[i + 1]))
            i = skip_groups(t, n, skip_digits(t, n, i + 1));
        if (i < n && t[i] == '%')
            ++i;
    }
    *to = i;
    if (i < n && t[i] == '\'') {
        ++i;
        int letters = i;
        while (i < n && (isalpha((unsigned char)t[i]) || t[i] == (char)0x80))
            ++i;
        if (i == letters)
            return false;
    }
    while (i < n && strchr(",.;:)]", t[i]) != NULL)
        ++i;
    return i == n;
}

/* A tool's summary starts its row with one of these words; its numbers are the interface's. */
static bool is_tool_word(const char *text, int length)
{
    static const char *const words[] = {"Read", "Reading", "Searched", "Searching", "Ran", "Running", "Listed", "Listing",
                                        "Wrote", "Writing", "Updated", "Updating", "Edited", "Editing", "Added", "Removed",
                                        "Fetched", "Fetching", "Found"};
    for (size_t i = 0; i < sizeof words / sizeof *words; ++i)
        if ((int)strlen(words[i]) == length && memcmp(words[i], text, (size_t)length) == 0)
            return true;
    return false;
}

static bool is_number_paint(int kind)
{
    return kind == PAINT_NUMBER || kind == PAINT_DATE;
}

/*
 * The number, date or time in the word at from..to of a row: its columns, when the word is one. The word
 * must stand in the program's default look, all bold or all not, with no joined marks.
 */
static bool find_word_number(const struct screen *sc, int row, int from, int to, int *number_from, int *number_to,
                             bool *is_date)
{
    int n = to - from, first_digit = 0, past_number = 0;
    const char *t = (const char *)&sc->text[row][from];
    unsigned char first = sc->cells[row][from];

    if (n > 48)
        return false;
    for (int column = from; column < to; ++column)
        if ((sc->cells[row][column] & CELL_PLAIN) == 0 || ((sc->cells[row][column] ^ first) & CELL_BOLD) != 0 ||
            (sc->glyphs[row][column] & GLYPH_JOINED) != 0)
            return false;
    if (!find_number(t, n, &first_digit, &past_number, is_date))
        return false;
    if (from <= 5 && first_digit == 0 && past_number == n - 1 && t[n - 1] == '.' && skip_digits(t, n, 0) == past_number)
        return false; /* the number of a list or a menu */
    *number_from = from + first_digit;
    *number_to = from + past_number;
    return true;
}

/*
 * A run of characters has ended. The whole word around it is read from the map, because a program may
 * rewrite only the digits that changed: a number gets its colour again from end to end, and cells that
 * no longer belong to a number get theirs back.
 */
static void end_word(struct screen *sc)
{
    int row = sc->word_row, from = sc->word_column, to = from + sc->word_length;

    if (sc->word_length == 0)
        return;
    sc->word_length = 0;
    if (row >= sc->rows || to > sc->columns || sc->has_saved_cursor || sc->is_insert || sc->is_origin)
        return; /* writing there would disturb the program */
    while (from > 0 && sc->text[row][from - 1] != 0)
        --from;
    while (to < sc->columns && sc->text[row][to] != 0)
        ++to;

    int n = to - from, paint_from = to, paint_to = to;
    const char *t = (const char *)&sc->text[row][from];

    if (from <= 2 && is_tool_word(t, n) && (sc->cells[row][from] & CELL_TOOL) == 0) {
        sc->cells[row][from] |= CELL_TOOL;
        sc->is_dirty = true;
    }

    bool is_date = false;
    /* Typed text keeps its own colour, also after an empty row of the box. */
    bool is_number = find_word_number(sc, row, from, to, &paint_from, &paint_to, &is_date) && !is_interface_row(sc, row) &&
                     !is_input_row(sc, row);

    if (!is_number)
        paint_from = paint_to = to;

    bool has_work = is_number;

    for (int column = from; column < to; ++column)
        if ((column < paint_from || column >= paint_to) && is_number_paint(paint_at(sc, row, column)))
            has_work = true;
    if (!has_work)
        return;

    put_text("\0337");
    for (int column = from; column < to; ++column) {
        if ((column < paint_from || column >= paint_to) && is_number_paint(paint_at(sc, row, column))) {
            put_move(row, column);
            put_text((sc->cells[row][column] & CELL_BOLD) != 0 ? "\033[0;1m" : "\033[0m");
            put(&sc->text[row][column], 1);
            set_paint(sc, row, column, PAINT_NONE);
        }
    }
    if (is_number) {
        put_move(row, paint_from);
        put_text((sc->cells[row][from] & CELL_BOLD) != 0 ? "\033[0;1;38;2;" : "\033[0;38;2;");
        put_text(is_date ? REPLY_DATE_COLOUR "m" : REPLY_NUMBER_COLOUR "m");
        put(&sc->text[row][paint_from], (size_t)(paint_to - paint_from));
        for (int column = paint_from; column < paint_to; ++column)
            set_paint(sc, row, column, is_date ? PAINT_DATE : PAINT_NUMBER);
    }
    put_text("\0338");
    sc->is_dirty = true; /* the frame's end looks at the row again */
}

static void draw_edge(const struct screen *sc, int row, const char *block)
{
    put_move(row, 0);
    put_text("\033[0;" BOX_COLOUR "m");
    for (int column = 0; column < sc->columns; ++column)
        put_text(block);
}

/* What the program thinks is blank there must be blank again. */
static void remove_edge(const struct screen *sc, int row)
{
    put_text("\033[0m");
    for (int column = 0; column < sc->columns; ++column) {
        if (sc->cells[row][column] == 0) {
            put_move(row, column);
            put_text(" ");
        }
    }
}

static void draw_sides(const struct screen *sc, int row)
{
    int last = sc->columns - 1;

    put_move(row, 0);
    put_text("\033[0;" BOX_COLOUR ";" BAND_BACKGROUND "m");
    /*
     * A used first cell keeps its character. The prompt mark stays in its own cell with the program's
     * blank after it, and the box's line gives way to it on that row (the author, 2026-11-08).
     */
    if ((sc->cells[row][0] & CELL_USED) == 0)
        put_text("\u258f");

    if ((sc->cells[row][last] & CELL_USED) == 0) {
        put_move(row, last);
        put_text("\u2595");
    }
}

static void draw_box(struct interpreter *in)
{
    struct screen *sc = &in->screen;
    unsigned char wanted[MAX_ROWS] = {NO_EDGE};
    bool has_work = sc->has_band_changed;

    for (int row = 0; row < sc->rows; ++row) {
        if (!is_band_row(sc, row))
            continue;
        if (row > 0 && is_blank_row(sc, row - 1))
            wanted[row - 1] = TOP_EDGE;
        if (row + 1 < sc->rows && is_blank_row(sc, row + 1) && wanted[row + 1] == NO_EDGE)
            wanted[row + 1] = BOTTOM_EDGE;
    }
    for (int row = 0; row < sc->rows; ++row)
        has_work = has_work || wanted[row] != sc->edges[row];
    if (!has_work || sc->has_saved_cursor || sc->is_insert || sc->is_origin)
        return; /* drawing then would take the program's saved cursor, or land elsewhere */

    put_text("\033[?2026h\0337");
    for (int row = 0; row < sc->rows; ++row) {
        if (wanted[row] != sc->edges[row]) {
            if (wanted[row] == TOP_EDGE)
                draw_edge(sc, row, "\u2581");
            else if (wanted[row] == BOTTOM_EDGE)
                draw_edge(sc, row, "\u2594");
            else
                remove_edge(sc, row);
            sc->edges[row] = wanted[row];
        }
    }
    if (sc->has_band_changed) {
        for (int row = 0; row < sc->rows; ++row)
            if (is_band_row(sc, row))
                draw_sides(sc, row);
    }
    sc->has_band_changed = false;
    put_text("\0338\033[?2026l");
}

/* Reply text. The functions up to settle_reply only read the maps and say what each cell should be painted as. */

/* A cell with nothing to read in it: a blank, or the blank a program writes as a character. */
static bool is_gap(const struct screen *sc, int row, int column)
{
    return (sc->cells[row][column] & CELL_USED) == 0 || glyph_at(sc, row, column) == 0xa0;
}

/* A cell berkeDark may write again: a known character in the program's default look, with nothing joined to it. */
static bool is_paintable(const struct screen *sc, int row, int column)
{
    unsigned cell = sc->glyphs[row][column];

    return (sc->cells[row][column] & CELL_PLAIN) != 0 && (cell & GLYPH_JOINED) == 0 &&
           (cell & (GLYPH_CODE | GLYPH_TAIL)) != 0;
}

/* Paintable and not bold. */
static bool is_light(const struct screen *sc, int row, int column)
{
    return is_paintable(sc, row, column) && (sc->cells[row][column] & CELL_BOLD) == 0;
}

static int first_text(const struct screen *sc, int row, int from)
{
    while (from < sc->columns && is_gap(sc, row, from))
        ++from;
    return from;
}

/* Where a row's text starts; the reply mark in the first column is passed over. */
static int row_start(const struct screen *sc, int row)
{
    return first_text(sc, row, (sc->cells[row][0] & CELL_REPLY) != 0 ? 1 : 0);
}

static bool is_line_glyph(unsigned glyph)
{
    return glyph >= 0x2500 && glyph <= 0x257f;
}

/* A row drawn with line characters at both ends: a row of a table, or of a box in a drawing. */
static bool is_table_row(const struct screen *sc, int row, int start)
{
    int last = sc->columns - 1;

    while (last > start && is_gap(sc, row, last))
        --last;
    return is_line_glyph(glyph_at(sc, row, start)) && is_line_glyph(glyph_at(sc, row, last));
}

/*
 * A table as Claude Code draws it. Its lines take the border colour. The row between the top line and the
 * first inner line holds the names of the columns.
 */
static void want_table_row(const struct screen *sc, int row, int start, unsigned char *want)
{
    bool is_header = glyph_at(sc, row, start) == 0x2502 && row > 0 && row + 1 < sc->rows &&
                     row_start(sc, row - 1) == start && glyph_at(sc, row - 1, start) == 0x250c &&
                     row_start(sc, row + 1) == start && glyph_at(sc, row + 1, start) == 0x251c;

    for (int column = start; column < sc->columns; ++column) {
        if (is_line_glyph(glyph_at(sc, row, column))) {
            if (is_light(sc, row, column))
                want[column] = PAINT_TABLE_BORDER;
        } else if (is_header && sc->text[row][column] != 0 && is_paintable(sc, row, column)) {
            want[column] = PAINT_TABLE_HEADER;
        }
    }
}

/*
 * A rule. Claude Code writes it as three hyphens alone on their row, with an empty row above and below.
 * It is drawn as a line across the width of the text, into cells the program left empty.
 */
static bool want_rule(const struct screen *sc, int row, int start, unsigned char *want)
{
    int end = start;

    while (end < sc->columns && glyph_at(sc, row, end) == '-' && is_light(sc, row, end))
        ++end;
    if (end - start != 3 || first_text(sc, row, end) < sc->columns)
        return false;
    if ((row > 0 && !is_blank_row(sc, row - 1)) || (row + 1 < sc->rows && !is_blank_row(sc, row + 1)))
        return false;
    for (int column = start; column < end; ++column)
        want[column] = PAINT_RULE;
    for (int column = end; column < sc->columns - 2; ++column)
        if (sc->cells[row][column] == 0)
            want[column] = PAINT_RULE;
    return true;
}

/*
 * The mark of a list item: a hyphen, or a number with a full stop, then one blank and the item's text.
 * A task's box after the hyphen gives way to the task's mark: the hyphen stays, so the list keeps its
 * shape, and the mark stands in the box's last cell, right before the task's text.
 * Returns where the item's text starts, or start when the row is no list item.
 */
static int want_list_mark(const struct screen *sc, int row, int start, unsigned char *want)
{
    int columns = sc->columns, digits = start;

    if (glyph_at(sc, row, start) == '-' && is_light(sc, row, start) && start + 2 < columns && is_gap(sc, row, start + 1) &&
        !is_gap(sc, row, start + 2)) {
        int box = start + 2;

        if (box + 3 < columns && glyph_at(sc, row, box) == '[' && is_light(sc, row, box) &&
            glyph_at(sc, row, box + 2) == ']' && is_light(sc, row, box + 2) && is_gap(sc, row, box + 3)) {
            unsigned inside = glyph_at(sc, row, box + 1);
            bool is_open = sc->cells[row][box + 1] == 0;
            bool is_done = !is_open && (inside == 'x' || inside == 'X') && is_light(sc, row, box + 1);

            if (is_open || is_done) {
                want[start] = PAINT_LIST_MARK;
                want[box] = PAINT_HIDDEN;
                if (is_done)
                    want[box + 1] = PAINT_HIDDEN;
                want[box + 2] = is_done ? PAINT_TASK_DONE : PAINT_TASK_OPEN;
                return box + 4;
            }
        }
        want[start] = PAINT_LIST_MARK;
        return start + 2;
    }

    while (digits < columns && isdigit(sc->text[row][digits]) && is_light(sc, row, digits))
        ++digits;
    if (digits > start && digits - start <= 3 && digits + 2 < columns && sc->text[row][digits] == '.' &&
        is_light(sc, row, digits) && is_gap(sc, row, digits + 1) && !is_gap(sc, row, digits + 2)) {
        for (int column = start; column <= digits; ++column)
            want[column] = PAINT_LIST_MARK;
        return digits + 2;
    }
    return start;
}

/*
 * A label: bold words that open the row or the list item and end with a colon, with one blank between
 * them at most. Text follows it on the row. A label that ends the row counts only after a list's mark:
 * without one the row is bold from end to end, a heading line.
 */
static bool want_label(const struct screen *sc, int row, int from, bool is_after_mark, unsigned char *want)
{
    int columns = sc->columns, end = from;

    for (int column = from; column < columns;) {
        if (!is_gap(sc, row, column)) {
            if ((sc->cells[row][column] & CELL_BOLD) == 0)
                break;
            end = ++column;
        } else if (column > from && column + 1 < columns && !is_gap(sc, row, column + 1) &&
                   (sc->cells[row][column + 1] & CELL_BOLD) != 0) {
            ++column;
        } else {
            break;
        }
    }
    if (end == from || glyph_at(sc, row, end - 1) != ':')
        return false;
    if (first_text(sc, row, end) < columns ? !is_gap(sc, row, end) : !is_after_mark)
        return false;
    for (int column = from; column < end; ++column)
        if (!is_gap(sc, row, column) && is_paintable(sc, row, column))
            want[column] = PAINT_LABEL;
    return true;
}

/* A heading line: a row that is bold from its first character to its last. */
static void want_heading_line(const struct screen *sc, int row, int start, unsigned char *want)
{
    bool has_paintable = false;

    for (int column = start; column < sc->columns; ++column) {
        if (is_gap(sc, row, column))
            continue;
        if ((sc->cells[row][column] & CELL_BOLD) == 0)
            return;
        has_paintable = has_paintable || is_paintable(sc, row, column);
    }
    for (int column = start; has_paintable && column < sc->columns; ++column)
        if (!is_gap(sc, row, column) && is_paintable(sc, row, column))
            want[column] = PAINT_HEADING_LINE;
}

/*
 * An address in a word: where it starts and where it ends. Brackets and quotes before it and closing
 * punctuation after it stay outside; a closing bracket belongs to it only when it opened inside it.
 */
static bool find_address(const unsigned char *t, int n, int *from, int *to)
{
    int start = 0, scheme = 0;

    while (start < n && strchr("(<[\"'", t[start]) != NULL)
        ++start;
    if (n - start > 8 && memcmp(t + start, "https://", 8) == 0)
        scheme = 8;
    else if (n - start > 7 && memcmp(t + start, "http://", 7) == 0)
        scheme = 7;
    else
        return false;

    int end = start + scheme;

    while (end < n && strchr("<>\"'`", t[end]) == NULL)
        ++end;
    while (end > start + scheme) {
        unsigned char last = t[end - 1];
        bool is_closing = (last == ')' && memchr(t + start, '(', (size_t)(end - 1 - start)) == NULL) ||
                          (last == ']' && memchr(t + start, '[', (size_t)(end - 1 - start)) == NULL);

        if (strchr(".,;:!?", last) == NULL && !is_closing)
            break;
        --end;
    }
    if (end <= start + scheme)
        return false;
    *from = start;
    *to = end;
    return true;
}

/*
 * Addresses and numbers in the words no other rule has taken. An address in round brackets that follows
 * other words is the address of a link, which Claude Code writes beside the link's words.
 */
static void want_words(const struct screen *sc, int row, int start, unsigned char *want)
{
    for (int from = start; from < sc->columns;) {
        if (sc->text[row][from] == 0) {
            ++from;
            continue;
        }

        int to = from, first = 0, past = 0;
        bool is_free = true, is_date = false;

        while (to < sc->columns && sc->text[row][to] != 0) {
            is_free = is_free && want[to] == PAINT_NONE;
            ++to;
        }
        if (is_free && find_address(&sc->text[row][from], to - from, &first, &past)) {
            bool is_link = from > start && first == 1 && sc->text[row][from] == '(' && from + past < to &&
                           sc->text[row][from + past] == ')';
            bool is_all_light = true;

            for (int column = from + first; column < from + past; ++column)
                is_all_light = is_all_light && is_light(sc, row, column);
            for (int column = from + first; is_all_light && column < from + past; ++column)
                want[column] = is_link ? PAINT_LINK_URL : PAINT_LINK_BARE;
        } else if (is_free && find_word_number(sc, row, from, to, &first, &past, &is_date)) {
            for (int column = first; column < past; ++column)
                want[column] = is_date ? PAINT_DATE : PAINT_NUMBER;
        }
        from = to;
    }
}

/* Whether the program drew the cell in a palette colour that berkeDark can write again. */
static bool is_coloured(const struct screen *sc, int row, int column)
{
    unsigned char look = sc->looks[row][column];

    return (look & LOOK_COLOUR) != 0 && (look & LOOK_OTHER) == 0;
}

/* Whether a coloured cell holds a character to write again; a blank inside a coloured run is left as it is. */
static bool is_repaintable(const struct screen *sc, int row, int column)
{
    unsigned cell = sc->glyphs[row][column];

    return (cell & GLYPH_JOINED) == 0 && ((cell & GLYPH_CODE) > ' ' || (cell & GLYPH_TAIL) != 0);
}

/* The cells from..to of a row as text: a blank for a cell with no word character. Returns the length without blanks at the end. */
static int read_cells(const struct screen *sc, int row, int from, int to, char *t, int size)
{
    int n = 0;

    for (int column = from; column < to && n < size - 1; ++column)
        t[n++] = sc->text[row][column] != 0 ? (char)sc->text[row][column] : ' ';
    while (n > 0 && t[n - 1] == ' ')
        --n;
    t[n] = '\0';
    return n;
}

static bool is_listed(const char *t, int n, const char *const *words)
{
    for (; *words != NULL; ++words)
        if ((int)strlen(*words) == n && memcmp(*words, t, (size_t)n) == 0)
            return true;
    return false;
}

/* Whether the name ends with a full stop and letters that name a kind of file. */
static bool has_known_extension(const char *t, int n)
{
    static const char *const extensions[] = {
        "c", "h", "cc", "cpp", "hpp", "py", "pyi", "js", "mjs", "cjs", "ts", "tsx", "jsx", "json", "jsonl", "md", "txt",
        "sh", "zsh", "bash", "toml", "yaml", "yml", "ini", "conf", "cfg", "lock", "log", "csv", "tsv", "xml", "html",
        "htm", "css", "svg", "png", "jpg", "jpeg", "gif", "webp", "pdf", "tex", "bib", "rs", "go", "java", "kt", "rb",
        "lua", "sql", "diff", "patch", "gz", "zip", "tar", "bak", "vsix", "bin", "so", "o", "a", "d", "in", "out", "mk",
        "cmake", "nix", "el", "vim", "ipynb", "docx", "xlsx", "pptx", "odt", "exe", "dll", "bat", "ps1", "php", "pl",
        "swift", "cs", "r", "m", "jl", "hs", "tmp", "env", "service", "desktop", "ttf", "otf", "mp4", "mp3", "wav",
        "ico", "bmp", "db", "sqlite", "pem", "crt", NULL};
    char extension[10];
    int dot = n - 1, length = 0;

    while (dot > 0 && t[dot] != '.' && t[dot] != '/')
        --dot;
    if (dot <= 0 || t[dot] != '.' || t[dot - 1] == '/' || n - dot - 1 < 1 || n - dot - 1 > 8)
        return false;
    for (int i = dot + 1; i < n; ++i)
        extension[length++] = (char)tolower((unsigned char)t[i]);
    return is_listed(extension, length, extensions);
}

/*
 * Whether a word of inline code is the path of a file: it starts as a path does (/, ~/, ./, ../), or it has
 * folders and ends in a known kind of file or has two folders at least, or it is a file's name with a known
 * kind. A place in the file may follow (:12, :12:5, :12-20). "m/s", "ve/veya" and a date are none.
 */
static bool is_path_text(const char *t, int n)
{
    int end = n, cut = n, slashes = 0, letters = 0;

    if (n < 2 || strstr(t, "://") != NULL)
        return false;
    while (cut > 0 && (isdigit((unsigned char)t[cut - 1]) || t[cut - 1] == '-' || t[cut - 1] == ':'))
        --cut;
    if (cut > 0 && cut < n - 1 && t[cut] == ':' && isdigit((unsigned char)t[cut + 1]))
        end = cut;
    for (int i = 0; i < end; ++i) {
        unsigned char c = (unsigned char)t[i];

        if (c == '/')
            ++slashes;
        else if (isalpha(c) || c >= 0x80)
            ++letters;
        else if (!isdigit(c) && strchr("._-~+@%,=", c) == NULL)
            return false;
    }
    if (letters == 0)
        return false;
    if (t[0] == '/' || strncmp(t, "~/", 2) == 0 || strncmp(t, "./", 2) == 0 || strncmp(t, "../", 3) == 0)
        return true;
    if (slashes > 0)
        return has_known_extension(t, end) || t[end - 1] == '/' || slashes >= 2;
    return has_known_extension(t, end);
}

/*
 * Code in a reply. Claude Code draws inline code, and a block with no language, in its bright blue; in a block
 * with a language its highlighter gives each word one of a few palette colours: keyword, literal and the name
 * of a class blue; the name of a function yellow; a built-in name and a key cyan, a type dim cyan; a string red;
 * a number and a comment green; a decorator and a preprocessor's row grey. Each run of one colour is read with
 * its text and gets berkeDark's colour for what it is. In a block of the diff language a removed row is red and
 * an added row green from their first character: those stay.
 */
static void want_code(const struct screen *sc, int row, int start, unsigned char *want)
{
    static const char *const literals[] = {"True", "False", "None", "true", "false", "null", "nil", "NULL", "nullptr",
                                           "undefined", "NaN", "Infinity", "TRUE", "FALSE", NULL};
    static const char *const class_words[] = {"class", "struct", "interface", "enum", "trait", "union", NULL};
    char t[96];
    int columns = sc->columns;
    bool is_class_row = false, is_after_class_word = false;
    bool is_directive_row = glyph_at(sc, row, start) == '#' && sc->looks[row][start] == (LOOK_COLOUR | 8);
    /* In a block of the diff language a removed row is red and an added row green from their first sign on. */
    bool is_removed_row = glyph_at(sc, row, start) == '-' && sc->looks[row][start] == (LOOK_COLOUR | 1);
    bool is_added_row = glyph_at(sc, row, start) == '+' && sc->looks[row][start] == (LOOK_COLOUR | 2);

    for (int from = start; from < columns;) {
        unsigned char look = sc->looks[row][from];
        int index = look & LOOK_INDEX, to = from + 1, kind = PAINT_NONE;

        if (!is_coloured(sc, row, from)) {
            ++from;
            continue;
        }
        while (to < columns && sc->looks[row][to] == look)
            ++to;

        int n = read_cells(sc, row, from, to, t, (int)sizeof t);
        bool is_dim = (look & LOOK_DIM) != 0;

        if (n > 0 && index == 4 && !is_dim) {
            /* Keywords, literals and the names of classes share blue: each word by itself. */
            for (int word = from; word < to;) {
                int end = word;

                while (end < to && sc->text[row][end] != 0)
                    ++end;
                if (end == word) {
                    ++word;
                    continue;
                }

                int length = read_cells(sc, row, word, end, t, (int)sizeof t);
                int word_kind = PAINT_CODE_KEYWORD;

                if (is_listed(t, length, literals)) {
                    word_kind = PAINT_CODE_CONSTANT;
                } else if (is_listed(t, length, class_words)) {
                    is_class_row = is_after_class_word = true;
                    word = end;
                    for (int column = word - length; column < end; ++column)
                        if (want[column] == PAINT_NONE && is_repaintable(sc, row, column))
                            want[column] = PAINT_CODE_KEYWORD;
                    continue;
                } else if (is_after_class_word || (is_class_row && isupper((unsigned char)t[0]))) {
                    word_kind = PAINT_CODE_CLASS;
                }
                is_after_class_word = false;
                for (int column = word; column < end; ++column)
                    if (want[column] == PAINT_NONE && is_repaintable(sc, row, column))
                        want[column] = (unsigned char)word_kind;
                word = end;
            }
            from = to;
            continue;
        }
        if (n > 0 && (!is_dim || index == 6)) {
            switch (index) {
            case 12:
                kind = memchr(t, ' ', (size_t)n) == NULL && is_path_text(t, n) ? PAINT_PATH : PAINT_CODE_INLINE;
                break;
            case 3:
                kind = PAINT_CODE_FUNCTION;
                break;
            case 6:
                if (is_dim)
                    kind = PAINT_CODE_TYPE;
                else if (t[0] == '"' || t[0] == '\'' || t[n - 1] == ':')
                    kind = PAINT_CODE_KEY;
                else
                    kind = to < columns && glyph_at(sc, row, to) == '(' ? PAINT_CODE_CALL : PAINT_CODE_TYPE;
                break;
            case 1:
                kind = is_removed_row ? PAINT_NONE : PAINT_CODE_STRING;
                break;
            case 2:
                if (is_added_row)
                    kind = PAINT_NONE;
                else if (isdigit((unsigned char)t[0]) || (n > 1 && strchr(".-+", t[0]) != NULL && isdigit((unsigned char)t[1])))
                    kind = PAINT_CODE_NUMBER;
                else
                    kind = PAINT_CODE_COMMENT;
                break;
            case 8:
                if (t[0] == '@' && n > 1 && (isalpha((unsigned char)t[1]) || t[1] == '_'))
                    kind = PAINT_CODE_DECORATOR;
                else if (is_directive_row)
                    kind = PAINT_CODE_MACRO;
                break;
            default:
                break;
            }
        }
        for (int column = from; kind != PAINT_NONE && column < to; ++column)
            if (want[column] == PAINT_NONE && is_repaintable(sc, row, column))
                want[column] = (unsigned char)kind;
        from = to;
    }
}

/* Whether a row holds words of a block of code: something in one of the highlighter's five colours. */
static bool has_code_words(const struct screen *sc, int row)
{
    for (int column = 0; column < sc->columns; ++column) {
        int index = sc->looks[row][column] & LOOK_INDEX;

        if (is_coloured(sc, row, column) && (index == 1 || index == 2 || index == 3 || index == 4 || index == 6))
            return true;
    }
    return false;
}

/* A row of plain signs only, such as a closing bracket. Inside a block of code it is code too. */
static bool is_bare_row(const struct screen *sc, int row)
{
    bool has_sign = false;

    for (int column = 0; column < sc->columns; ++column) {
        unsigned char cell = sc->cells[row][column], byte = sc->text[row][column];

        if ((cell & CELL_USED) == 0)
            continue;
        if ((cell & (CELL_PLAIN | CELL_BOLD)) != CELL_PLAIN || byte >= 0x80 || isalnum(byte))
            return false;
        has_sign = true;
    }
    return has_sign;
}

/* A cell of a command line that berkeDark may write again: plain text, or the grey of a running tool's line. */
static bool is_command_cell(const struct screen *sc, int row, int column)
{
    unsigned char look = sc->looks[row][column];
    unsigned cell = sc->glyphs[row][column];

    if ((cell & GLYPH_JOINED) != 0 || ((cell & GLYPH_CODE) <= ' ' && (cell & GLYPH_TAIL) == 0))
        return false;
    return is_light(sc, row, column) || look == (LOOK_COLOUR | 8);
}

/*
 * The parts of a command line in the cells from..to. The first word, and the first word after a sign that
 * joins commands, is a command's name; a word with a hyphen first is an option; quoted text, a variable,
 * a number and a path are read from their shape; text after a hash that starts a word is a comment.
 */
static void want_command(const struct screen *sc, int row, int from, int to, unsigned char *want)
{
    static const char signs[] = "|&;<>";
    char t[96];
    bool wants_name = true;

    for (int column = from; column < to;) {
        unsigned glyph = glyph_at(sc, row, column);
        int end = column + 1, kind;

        if (is_gap(sc, row, column)) {
            ++column;
            continue;
        }
        if (glyph == '"' || glyph == '\'') {
            while (end < to && glyph_at(sc, row, end) != glyph)
                ++end;
            if (end < to)
                ++end;
            kind = PAINT_CODE_STRING;
            wants_name = false;
        } else if (glyph < 0x80 && strchr(signs, (int)glyph) != NULL) {
            while (end < to && glyph_at(sc, row, end) > ' ' && glyph_at(sc, row, end) < 0x80 &&
                   strchr(signs, (int)glyph_at(sc, row, end)) != NULL)
                ++end;
            kind = PAINT_COMMAND_OPERATOR;
            wants_name = glyph == '|' || glyph == '&' || glyph == ';';
        } else {
            while (end < to && !is_gap(sc, row, end) &&
                   !(glyph_at(sc, row, end) < 0x80 && strchr("|&;<>\"'", (int)glyph_at(sc, row, end)) != NULL))
                ++end;

            int n = read_cells(sc, row, column, end, t, (int)sizeof t), digits = 0;

            while (digits < n && (isdigit((unsigned char)t[digits]) || t[digits] == '.' || t[digits] == ','))
                ++digits;
            if (t[0] == '#') {
                end = to;
                kind = PAINT_CODE_COMMENT;
            } else if (wants_name && memchr(t, '=', (size_t)n) == NULL) {
                kind = PAINT_COMMAND_NAME;
                wants_name = false;
            } else if (t[0] == '-' && n > 1) {
                kind = PAINT_COMMAND_OPTION;
            } else if (t[0] == '$') {
                kind = PAINT_COMMAND_VARIABLE;
            } else if (n > 0 && digits == n && isdigit((unsigned char)t[0])) {
                kind = PAINT_CODE_NUMBER;
            } else if (t[0] == '~' || memchr(t, '/', (size_t)n) != NULL || is_path_text(t, n)) {
                kind = PAINT_PATH;
            } else {
                kind = PAINT_COMMAND_TEXT;
            }
        }
        for (int cell = column; cell < end; ++cell) {
            if (!is_command_cell(sc, row, cell) || (kind == PAINT_COMMAND_TEXT && !is_coloured(sc, row, cell)))
                continue;
            want[cell] = (unsigned char)kind;
        }
        column = end;
    }
}

/*
 * Rows that are no reply text but hold what a tool was given or what the user types (the pool's fourth step):
 * shell mode in the input box ("!" under the box's rule) is a command line; the dim words offered in the box
 * take the suggestion colour; a tool's heading "Name(...)" shows a command for Bash and else paths and
 * quoted text; the grey line of a running tool is a command after "$", or a path.
 */
static void want_interface_row(const struct screen *sc, int row, unsigned char *want)
{
    int columns = sc->columns, start = first_text(sc, row, 0);
    bool is_shell = glyph_at(sc, row, 0) == '!' && sc->looks[row][0] == (LOOK_COLOUR | 9) && row > 0 && is_rule_row(sc, row - 1);
    char t[96];

    if (start >= columns)
        return;
    if (is_shell || (sc->cells[row][0] & CELL_INPUT) != 0) {
        for (int column = 2; column < columns; ++column)
            if (sc->looks[row][column] == LOOK_DIM && (glyph_at(sc, row, column) > ' ' || (sc->glyphs[row][column] & GLYPH_TAIL) != 0) &&
                (sc->glyphs[row][column] & GLYPH_JOINED) == 0)
                want[column] = PAINT_SUGGESTION;
        if (is_shell)
            want_command(sc, row, 2, columns, want);
        return;
    }
    if (glyph_at(sc, row, start) == 0x23bf) {
        int from = first_text(sc, row, start + 1), end = columns;

        while (end > from && is_gap(sc, row, end - 1))
            --end;
        if (from < end && glyph_at(sc, row, from) == '$' && from + 1 < end && is_gap(sc, row, from + 1)) {
            want_command(sc, row, from + 2, end, want);
        } else if (from < end && sc->looks[row][from] == (LOOK_COLOUR | 8)) {
            int n = read_cells(sc, row, from, end, t, (int)sizeof t);

            for (int column = from; n > 0 && memchr(t, ' ', (size_t)n) == NULL && is_path_text(t, n) && column < end; ++column)
                if (is_command_cell(sc, row, column))
                    want[column] = PAINT_PATH;
        }
        return;
    }
    if (start == 0 && glyph_at(sc, row, 0) == 0x25cf && (sc->cells[row][0] & CELL_REPLY) == 0 && columns > 4 &&
        (sc->cells[row][2] & CELL_BOLD) != 0) {
        int name_end = 2, end = columns;

        while (name_end < columns && !is_gap(sc, row, name_end) && (sc->cells[row][name_end] & CELL_BOLD) != 0)
            ++name_end;
        while (end > name_end && is_gap(sc, row, end - 1))
            --end;
        if (name_end >= end || glyph_at(sc, row, name_end) != '(')
            return;
        if (glyph_at(sc, row, end - 1) == ')')
            --end;

        int n = read_cells(sc, row, 2, name_end, t, (int)sizeof t);

        if (n == 4 && memcmp(t, "Bash", 4) == 0) {
            want_command(sc, row, name_end + 1, end, want);
            return;
        }
        for (int from = name_end + 1; from < end;) {
            unsigned glyph = glyph_at(sc, row, from);
            int to = from + 1;

            if (is_gap(sc, row, from)) {
                ++from;
                continue;
            }
            if (glyph == '"') {
                while (to < end && glyph_at(sc, row, to) != '"')
                    ++to;
                if (to < end)
                    ++to;
                for (int column = from; column < to; ++column)
                    if (is_command_cell(sc, row, column))
                        want[column] = PAINT_CODE_STRING;
            } else {
                while (to < end && !is_gap(sc, row, to) && glyph_at(sc, row, to) != ',')
                    ++to;
                n = read_cells(sc, row, from, to, t, (int)sizeof t);
                for (int column = from; n > 0 && is_path_text(t, n) && column < to; ++column)
                    if (is_command_cell(sc, row, column))
                        want[column] = PAINT_PATH;
            }
            from = to;
        }
    }
}

/* What each cell of a row should be painted as. */
static void want_row(const struct screen *sc, int row, bool is_reply_text, bool is_code_row, unsigned char *want)
{
    for (int column = 0; column < sc->columns; ++column) {
        int kind = paint_at(sc, row, column);

        want[column] = (unsigned char)(!is_reply_text && is_number_paint(kind) ? kind : PAINT_NONE);
    }
    if (!is_reply_text) {
        /* Outside reply text the numbers the word logic painted stay, and a few rows of the interface are read. */
        want_interface_row(sc, row, want);
        return;
    }

    int start = row_start(sc, row);

    if (start >= sc->columns)
        return;
    if (glyph_at(sc, row, start) == 0x258e) {
        /* A quoted row: its text takes its colour as it is printed; code inside it is read like any other. */
    } else if (is_table_row(sc, row, start)) {
        want_table_row(sc, row, start, want);
    } else if (!want_rule(sc, row, start, want)) {
        int body = want_list_mark(sc, row, start, want);

        if (!want_label(sc, row, body, body != start, want) && body == start)
            want_heading_line(sc, row, start, want);
    }
    want_code(sc, row, start, want);
    want_words(sc, row, start, want);
    /* The signs of plain code text, as on a diff's rows. */
    for (int column = start; is_code_row && column < sc->columns; ++column) {
        unsigned glyph = glyph_at(sc, row, column);

        if (want[column] == PAINT_NONE && glyph > ' ' && glyph < 0x80 && strchr(SYNTAX_PUNCTUATION, (int)glyph) != NULL &&
            is_light(sc, row, column))
            want[column] = PAINT_CODE_PUNCTUATION;
    }
}

/* What a walk down the screen knows when it reaches a row. */
struct walk {
    bool is_under_tool;  /* under a tool's result or summary, as is_interface_row finds it */
    bool is_in_input;    /* inside the input box, as is_input_row finds it */
    int rows_of_input;
    bool is_under_reply; /* the block's first row carries the reply mark, is the user's, or lies above the screen */
};

/* Takes the walk one row down and says whether that row is reply text. */
static bool follow_walk(const struct screen *sc, int row, struct walk *walk)
{
    unsigned char first = sc->cells[row][0];
    bool has_tool = (first & CELL_POINTER) != 0;

    for (int column = 0; column < 6 && column < sc->columns; ++column)
        has_tool = has_tool || (sc->cells[row][column] & CELL_TOOL) != 0;
    if (has_tool)
        walk->is_under_tool = true;
    else if ((first & CELL_USED) != 0 || is_blank_row(sc, row))
        walk->is_under_tool = false;

    if ((first & CELL_INPUT) != 0) {
        walk->is_in_input = true;
        walk->rows_of_input = 0;
    } else if ((first & CELL_USED) != 0 || (walk->is_in_input && ++walk->rows_of_input >= 64)) {
        walk->is_in_input = false;
    }

    /*
     * The user's row says nothing about the rows under it: while a long reply scrolls, Claude Code keeps the
     * user's prompt on the top row, right above the reply's text.
     */
    if (first != 0)
        walk->is_under_reply = (first & (CELL_REPLY | CELL_BAND)) != 0;

    return walk->is_under_reply && !walk->is_under_tool && !walk->is_in_input && sc->code_from[row] < 0 &&
           (first == 0 || (first & CELL_REPLY) != 0) && sc->cells[row][1] == 0;
}

static void put_code_point(unsigned code_point)
{
    unsigned char bytes[4];
    size_t length = 0;

    if (code_point < 0x80) {
        bytes[length++] = (unsigned char)code_point;
    } else if (code_point < 0x800) {
        bytes[length++] = (unsigned char)(0xc0 | code_point >> 6);
        bytes[length++] = (unsigned char)(0x80 | (code_point & 0x3f));
    } else if (code_point < 0x10000) {
        bytes[length++] = (unsigned char)(0xe0 | code_point >> 12);
        bytes[length++] = (unsigned char)(0x80 | (code_point >> 6 & 0x3f));
        bytes[length++] = (unsigned char)(0x80 | (code_point & 0x3f));
    } else {
        bytes[length++] = (unsigned char)(0xf0 | code_point >> 18);
        bytes[length++] = (unsigned char)(0x80 | (code_point >> 12 & 0x3f));
        bytes[length++] = (unsigned char)(0x80 | (code_point >> 6 & 0x3f));
        bytes[length++] = (unsigned char)(0x80 | (code_point & 0x3f));
    }
    put(bytes, length);
}

/* The colour of a paint kind, as the arguments after 38;2;. NULL: the kind keeps the program's own look. */
static const char *paint_colour(int kind)
{
    switch (kind) {
    case PAINT_NUMBER: return REPLY_NUMBER_COLOUR;
    case PAINT_DATE: return REPLY_DATE_COLOUR;
    case PAINT_HEADING_LINE: return HEADING_LINE_COLOUR;
    case PAINT_LABEL: return LABEL_COLOUR;
    case PAINT_LINK_URL: return LINK_URL_COLOUR;
    case PAINT_LINK_BARE: return LINK_BARE_COLOUR;
    case PAINT_LIST_MARK: return LIST_MARK_COLOUR;
    case PAINT_TASK_OPEN: return TASK_OPEN_COLOUR;
    case PAINT_TASK_DONE: return TASK_DONE_COLOUR;
    case PAINT_TABLE_BORDER: return TABLE_BORDER_COLOUR;
    case PAINT_TABLE_HEADER: return TABLE_HEADER_COLOUR;
    case PAINT_RULE: return RULE_COLOUR;
    case PAINT_CODE_INLINE: return CODE_INLINE_COLOUR;
    case PAINT_PATH: return PATH_COLOUR;
    case PAINT_CODE_KEYWORD: return CODE_KEYWORD_COLOUR;
    case PAINT_CODE_CONSTANT: return CODE_CONSTANT_COLOUR;
    case PAINT_CODE_CLASS: return CODE_CLASS_COLOUR;
    case PAINT_CODE_FUNCTION: return CODE_FUNCTION_COLOUR;
    case PAINT_CODE_TYPE: return CODE_TYPE_COLOUR;
    case PAINT_CODE_CALL: return CODE_CALL_COLOUR;
    case PAINT_CODE_KEY: return CODE_KEY_COLOUR;
    case PAINT_CODE_STRING: return CODE_STRING_COLOUR;
    case PAINT_CODE_NUMBER: return CODE_NUMBER_COLOUR;
    case PAINT_CODE_COMMENT: return CODE_COMMENT_COLOUR;
    case PAINT_CODE_DECORATOR: return CODE_DECORATOR_COLOUR;
    case PAINT_CODE_MACRO: return CODE_MACRO_COLOUR;
    case PAINT_CODE_PUNCTUATION: return CODE_PUNCTUATION_COLOUR;
    case PAINT_COMMAND_NAME: return COMMAND_NAME_COLOUR;
    case PAINT_COMMAND_OPTION: return COMMAND_OPTION_COLOUR;
    case PAINT_COMMAND_VARIABLE: return COMMAND_VARIABLE_COLOUR;
    case PAINT_COMMAND_OPERATOR: return COMMAND_OPERATOR_COLOUR;
    case PAINT_COMMAND_TEXT: return COMMAND_TEXT_COLOUR;
    case PAINT_SUGGESTION: return SUGGESTION_COLOUR;
    default: return NULL;
    }
}

/* A style in one number: the paint kind, the marks, and the program's palette colour when the kind keeps it. */
enum { STYLE_KIND = 0x3f, STYLE_BOLD = 0x100, STYLE_DIM = 0x200, STYLE_ITALIC = 0x400, STYLE_PALETTE = 0x800, STYLE_INDEX_SHIFT = 12 };

/*
 * The style a paint kind gives a cell. A kind with a colour keeps the cell's bold and italic, drops its dim
 * and adds its own marks. PAINT_NONE and PAINT_HIDDEN give the style the program drew the cell in.
 */
static unsigned paint_style(const struct screen *sc, int row, int column, int kind)
{
    unsigned char look = sc->looks[row][column];
    unsigned style = (sc->cells[row][column] & CELL_BOLD) != 0 ? STYLE_BOLD : 0;

    if ((look & LOOK_ITALIC) != 0)
        style |= STYLE_ITALIC;
    if (paint_colour(kind) != NULL) {
        if (kind == PAINT_TABLE_HEADER)
            style |= STYLE_BOLD;
        if (kind == PAINT_CODE_COMMENT || kind == PAINT_CODE_DECORATOR || kind == PAINT_COMMAND_OPTION)
            style |= STYLE_ITALIC;
        return style | (unsigned)kind;
    }
    if ((look & LOOK_DIM) != 0)
        style |= STYLE_DIM;
    if ((look & LOOK_COLOUR) != 0)
        style |= STYLE_PALETTE | (unsigned)(look & LOOK_INDEX) << STYLE_INDEX_SHIFT;
    return style;
}

static void put_style(unsigned style)
{
    char text[64];
    int kind = (int)(style & STYLE_KIND);
    const char *colour = (style & STYLE_BOLD) == 0 ? paint_colour(kind)
                         : kind == PAINT_CODE_INLINE ? CODE_INLINE_BOLD_COLOUR
                         : kind == PAINT_PATH ? PATH_BOLD_COLOUR
                         : paint_colour(kind);
    int index = (int)(style >> STYLE_INDEX_SHIFT & 0xf);
    int length = snprintf(text, sizeof text, "\033[0%s%s%s", (style & STYLE_BOLD) != 0 ? ";1" : "",
                          (style & STYLE_DIM) != 0 ? ";2" : "", (style & STYLE_ITALIC) != 0 ? ";3" : "");

    if (colour != NULL)
        length += snprintf(text + length, sizeof text - (size_t)length, ";38;2;%s", colour);
    else if ((style & STYLE_PALETTE) != 0)
        length += snprintf(text + length, sizeof text - (size_t)length, ";%d", index < 8 ? 30 + index : 82 + index);
    text[length++] = 'm';
    put(text, (size_t)length);
}

static void put_painted_glyph(const struct screen *sc, int row, int column, int kind)
{
    unsigned glyph = glyph_at(sc, row, column);

    switch (kind) {
    case PAINT_TASK_OPEN: put_text(TASK_OPEN_MARK); break;
    case PAINT_TASK_DONE: put_text(TASK_DONE_MARK); break;
    case PAINT_RULE: put_text(RULE_MARK); break;
    case PAINT_HIDDEN: put_text(" "); break;
    default: put_code_point(glyph != 0 ? glyph : ' '); break;
    }
}

/*
 * The end of a frame. Every row is read again, and each cell whose paint is not what its row asks for is
 * written again: in its colour, or back in the program's own look. It goes on top of the program's frame,
 * like the box, and the cursor and the program's style come back afterwards.
 */
static void settle_reply(struct interpreter *in)
{
    enum { ROW_REPLY = 1, ROW_BLANK = 2, ROW_WORDS = 4, ROW_BARE = 8, ROW_CODE = 16 };
    static unsigned char want[MAX_COLUMNS], facts[MAX_ROWS];
    struct screen *sc = &in->screen;
    struct walk walk = {.is_under_reply = true};
    bool has_begun = false;

    if (!sc->is_claude || !sc->is_dirty || sc->has_saved_cursor || sc->is_insert || sc->is_origin)
        return; /* writing then would take the program's saved cursor, or land elsewhere */
    sc->is_dirty = false;

    /*
     * Which rows are reply text, and which of those are rows of a block of code: the rows that hold coloured
     * words of code, and rows of plain signs that stand with them, no empty row between.
     */
    for (int row = 0; row < sc->rows; ++row) {
        facts[row] = follow_walk(sc, row, &walk) ? ROW_REPLY : 0;
        if (is_blank_row(sc, row))
            facts[row] |= ROW_BLANK;
        else if ((facts[row] & ROW_REPLY) != 0)
            facts[row] |= has_code_words(sc, row) ? ROW_WORDS : is_bare_row(sc, row) ? ROW_BARE : 0;
    }
    for (int row = 0; row < sc->rows;) {
        int end = row;
        bool has_words = false;

        while (end < sc->rows && (facts[end] & (ROW_REPLY | ROW_BLANK)) == ROW_REPLY) {
            if ((facts[end] & ROW_WORDS) != 0)
                has_words = true;
            ++end;
        }
        for (int group_row = row; has_words && group_row < end; ++group_row)
            if ((facts[group_row] & (ROW_WORDS | ROW_BARE)) != 0)
                facts[group_row] |= ROW_CODE;
        row = end > row ? end : row + 1;
    }

    for (int row = 0; row < sc->rows; ++row) {
        unsigned style = ~0u;
        int cursor = -1; /* the column the terminal's cursor stands at on this row, when berkeDark put it there */

        want_row(sc, row, (facts[row] & ROW_REPLY) != 0, (facts[row] & ROW_CODE) != 0, want);
        for (int column = 0; column < sc->columns; ++column) {
            unsigned cell = sc->glyphs[row][column];
            int kind = want[column];

            if (kind == paint_at(sc, row, column) || (cell & GLYPH_JOINED) != 0)
                continue;
            if ((cell & GLYPH_TAIL) != 0) {
                set_paint(sc, row, column, kind); /* written with the first cell of its character */
                continue;
            }
            if (!has_begun) {
                put_text("\033[?2026h\0337");
                has_begun = true;
            }
            if (cursor != column)
                put_move(row, column);

            unsigned wanted_style = paint_style(sc, row, column, kind);
            bool is_wide = column + 1 < sc->columns && (sc->glyphs[row][column + 1] & GLYPH_TAIL) != 0;

            if (wanted_style != style) {
                put_style(wanted_style);
                style = wanted_style;
            }
            put_painted_glyph(sc, row, column, kind);
            set_paint(sc, row, column, kind);
            cursor = column + (is_wide ? 2 : 1);
        }
    }
    if (has_begun)
        put_text("\0338\033[?2026l");
}

/* Whether the program in the foreground of the pseudo-terminal is Claude Code, by its process name (Linux). */
static bool foreground_is_claude(void)
{
    if (program_master < 0)
        return true;

    char path[64], name[32] = "";
    pid_t group = tcgetpgrp(program_master);

    if (group <= 0)
        return false;
    snprintf(path, sizeof path, "/proc/%d/comm", (int)group);

    FILE *file = fopen(path, "r");

    if (file == NULL)
        return false;
    bool has_name = fgets(name, sizeof name, file) != NULL;
    fclose(file);
    return has_name && strcmp(name, "claude\n") == 0;
}

static void apply_private_modes(struct interpreter *in, bool is_on)
{
    struct screen *sc = &in->screen;

    for (const char *p = in->parameters; p != NULL; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL) {
        int mode = read_parameter(p);

        if (mode == 1049 || mode == 1047 || mode == 47) {
            sc->is_alternate = is_on;
            sc->is_claude = is_on && foreground_is_claude();
            clear_map(sc);
        } else if (mode == 6) {
            sc->is_origin = is_on;
            sc->row = is_on ? sc->top : 0; /* measured on foot: either way the cursor goes home */
            sc->column = 0;
        } else if (mode == 7) {
            sc->is_no_wrap = !is_on;
        } else if (mode == 2026 && !is_on && has_map(sc)) {
            draw_box(in);
            settle_reply(in);
        }
    }
}

static void finish_control_sequence(struct interpreter *in, unsigned char final)
{
    if (!in->is_plain)
        return;

    in->parameters[in->length] = '\0';

    if (in->is_private) {
        if (final == 'h' || final == 'l')
            apply_private_modes(in, final == 'h');
        return;
    }

    if (final == 'm') {
        apply_style_sequence(in);
        return;
    }
    if (final == 's' && in->length == 0)
        in->saved_style = in->style;
    else if (final == 'u' && in->length == 0)
        in->style = in->saved_style;
    if (has_map(&in->screen))
        follow_cursor_sequence(in, final);
}

/* Follows the stream one byte at a time, so a sequence may span two reads. */
static void track(struct interpreter *in, unsigned char c)
{
    if (has_map(&in->screen) && in->state != STRING && c < 0x20 && c != 0x1b)
        read_control_character(&in->screen, c);

    switch (in->state) {
    case GROUND:
        if (c == 0x1b)
            in->state = ESCAPE;
        else if (c >= 0x20 && c != 0x7f && has_map(&in->screen))
            read_text(in, c);
        break;

    case ESCAPE:
        in->state = GROUND;
        if (c == '[') {
            in->state = CONTROL_SEQUENCE;
            in->length = 0;
            in->is_plain = true;
            in->is_private = false;
        } else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
            in->state = STRING;
            in->bell_ends_string = c == ']';
        } else if (c == 0x1b) {
            in->state = ESCAPE;
        } else if (c >= 0x20 && c <= 0x2f) {
            in->state = ESCAPE_INTERMEDIATE;
        } else if (c == '7') {
            in->saved_style = in->style;
            in->screen.saved_row = in->screen.row;
            in->screen.saved_column = in->screen.column;
            in->screen.has_saved_cursor = true;
        } else if (c == '8') {
            in->style = in->saved_style;
            in->screen.row = in->screen.saved_row;
            in->screen.column = in->screen.saved_column;
            in->screen.has_saved_cursor = false;
        } else if (c == 'c') {
            in->style = (struct style){0};
            clear_map(&in->screen);
            in->screen.is_alternate = false;
            in->screen.is_no_wrap = false;
        } else if (c == 'M' && has_map(&in->screen)) {
            reverse_index(&in->screen);
        } else if (c == 'D' && has_map(&in->screen)) {
            line_feed(&in->screen);
        } else if (c == 'E' && has_map(&in->screen)) {
            in->screen.column = 0;
            line_feed(&in->screen);
        }
        break;

    case ESCAPE_INTERMEDIATE:
        if (c == 0x1b)
            in->state = ESCAPE;
        else if (c >= 0x30 && c <= 0x7e)
            in->state = GROUND;
        break;

    case CONTROL_SEQUENCE:
        if (c >= 0x40 && c <= 0x7e) {
            in->state = GROUND;
            finish_control_sequence(in, c);
        } else if (c == 0x1b) {
            in->state = ESCAPE;
        } else if (c == 0x18 || c == 0x1a) {
            in->state = GROUND;
        } else if (c == '?' && in->length == 0 && in->is_plain) {
            in->is_private = true;
        } else if (c >= 0x20) {
            bool fits = in->length + 1 < sizeof in->parameters;
            if (fits && (isdigit(c) || c == ';' || c == ':'))
                in->parameters[in->length++] = (char)c;
            else
                in->is_plain = false;
        }
        break;

    case STRING:
        /* ESC ends the string; the backslash of ST then ends the escape. */
        if (c == 0x1b)
            in->state = ESCAPE;
        else if (c == 0x18 || c == 0x1a || (c == 0x07 && in->bell_ends_string))
            in->state = GROUND;
        break;
    }
}

/* The program's own colour mark, written again when a character leaves berkeDark's syntax colour. */
static void put_program_colour(const struct style *s)
{
    char text[64];
    int length;

    if (s->foreground == 38)
        length = snprintf(text, sizeof text, "\033[38%sm", s->extended);
    else if (s->foreground != 0)
        length = snprintf(text, sizeof text, "\033[%dm", s->foreground);
    else
        length = snprintf(text, sizeof text, "\033[39m");
    put(text, (size_t)length);
}

/* Whether the cursor stands right of a quote bar that starts its row: Claude Code draws the bar on every quoted row. */
static bool is_after_quote_bar(const struct screen *sc)
{
    for (int column = 0; column < sc->column && column < sc->columns; ++column) {
        if (is_gap(sc, sc->row, column) || (sc->cells[sc->row][column] & CELL_REPLY) != 0)
            continue;
        return glyph_at(sc, sc->row, column) == 0x258e;
    }
    return false;
}

/* The letters that may stand right before a string's opening quote: f"..", rb'..', L'x', u8"..". */
static bool is_string_prefix(const unsigned char *t, int length)
{
    if (length < 1 || length > 2)
        return false;
    for (int i = 0; i < length; ++i)
        if (strchr("fFrRbBuUL8nNxXeE", t[i]) == NULL)
            return false;
    return true;
}

/*
 * Whether the character about to be printed on a diff's row belongs to a "string" that an apostrophe began:
 * the string colour starts at a single quote that stands right after a word (the buffer's, don't), which no
 * string does except after a prefix letter or two. The cells of such a run carry a mark, so a part of the
 * run that the program writes again later is known by its neighbour.
 */
static bool is_apostrophe_text(const struct interpreter *in, unsigned char c)
{
    const struct style *s = &in->style;
    const struct screen *sc = &in->screen;
    int row = sc->row, column = sc->column;

    if (!has_map(sc) || !sc->is_claude || s->dim || s->foreground != 92 || row >= sc->rows || column >= sc->columns ||
        sc->code_from[row] < 0 || column <= sc->code_from[row])
        return false;
    if (!s->is_colour_start || (c != '\'' && c != '"' && c != '`')) {
        /*
         * Such a run goes on, also when a part of it is written again later, and also across blanks the
         * program jumped over instead of printing them.
         */
        int left = column - 1;

        while (left > sc->code_from[row] && sc->cells[row][left] == 0 && (sc->glyphs[row][left] & GLYPH_APOSTROPHE) == 0)
            --left;
        return (sc->glyphs[row][left] & GLYPH_APOSTROPHE) != 0;
    }
    if (c != '\'')
        return false;

    /* The string colour starts at a single quote: an apostrophe when a word stands right before it. */
    const unsigned char *t = sc->text[row];
    int from = column;

    while (from > sc->code_from[row] && (isalnum(t[from - 1]) || t[from - 1] == '_' || t[from - 1] == 0x80))
        --from;
    return from < column && !is_string_prefix(t + from, column - from);
}

/* The kind of a diff's row, from the mark in its gutter: '+' added, '-' removed, 0 not known. */
static char diff_row_kind(const struct screen *sc, int row)
{
    for (int column = 0; column < sc->code_from[row] && column < sc->columns; ++column) {
        unsigned glyph = glyph_at(sc, row, column);

        if (glyph == '+' || glyph == '-')
            return (char)glyph;
    }
    return 0;
}

/*
 * The ground berkeDark wants under the character about to be printed: on a diff's row, where the program's
 * background is one of its greys.
 */
static const char *wanted_ground(const struct interpreter *in)
{
    const struct style *s = &in->style;
    const struct screen *sc = &in->screen;
    char kind = 0;

    if (!has_map(sc) || !sc->is_claude || s->ground == 0 || sc->row >= sc->rows)
        return NULL;
    /*
     * The gutter is written in its mark's colour, and the row's code start in the map may still be the one of
     * the row that stood here before: bright red on such a ground is a removed row's gutter, bright green an
     * added row's gutter or a string on an added row. Everything else asks the mark in the map.
     */
    if (!s->dim && (s->foreground == 91 || s->foreground == 92))
        kind = s->foreground == 91 ? '-' : '+';
    else if (sc->code_from[sc->row] >= 0)
        kind = diff_row_kind(sc, sc->row);
    if (kind == '+')
        return s->ground == 235 ? DIFF_ADDED_GROUND : DIFF_ADDED_WORD_GROUND;
    if (kind == '-')
        return s->ground == 235 ? DIFF_REMOVED_GROUND : DIFF_REMOVED_WORD_GROUND;
    return NULL;
}

static void settle_ground(struct interpreter *in)
{
    struct style *s = &in->style;
    const char *wanted = wanted_ground(in);
    char text[32];

    if (wanted == s->ground_on)
        return;
    /* Leaving berkeDark's ground, the program's grey is written again: its own mark is still the last one it sent. */
    int length = wanted != NULL ? snprintf(text, sizeof text, "\033[%sm", wanted)
                                : snprintf(text, sizeof text, "\033[48;5;%dm", s->ground);

    put(text, (size_t)length);
    s->ground_on = wanted;
}

static const char punctuation_colour[] = SYNTAX_PUNCTUATION_COLOUR;

static const char *wanted_syntax(const struct interpreter *in, unsigned char c)
{
    const struct style *s = &in->style;
    const struct screen *sc = &in->screen;

    if (!has_map(sc) || !sc->is_claude || s->dim || sc->row >= sc->rows)
        return NULL;
    /* Text typed into the input box: after the mark and its blank, where the program chose no colour. */
    if (s->foreground == 0 && !s->background && !s->reverse && s->tint == NULL && sc->column >= 2 &&
        sc->code_from[sc->row] < 0 && is_input_row(sc, sc->row))
        return INPUT_TEXT_COLOUR;
    /* Quoted text: the italic text beside a quote bar, bold or not; a top heading keeps its own colour. */
    if (s->italic && s->tint != NULL && !(s->bold && s->underline) && !s->background && !s->reverse &&
        is_after_quote_bar(sc))
        return QUOTE_TEXT_COLOUR;
    if (sc->code_from[sc->row] < 0 || sc->column < sc->code_from[sc->row])
        return NULL;
    switch (s->foreground) {
    case 95: return SYNTAX_KEYWORD_COLOUR;
    case 94: return SYNTAX_NUMBER_COLOUR;
    case 92:
        if (s->is_apostrophe)
            return c < 0x80 && strchr(SYNTAX_PUNCTUATION, c) != NULL ? punctuation_colour : SYNTAX_TEXT_COLOUR;
        return SYNTAX_STRING_COLOUR;
    case 93: return SYNTAX_FUNCTION_COLOUR;
    case 37: return c < 0x80 && strchr(SYNTAX_PUNCTUATION, c) != NULL ? punctuation_colour : NULL;
    default: return NULL;
    }
}

static void settle_syntax(struct interpreter *in, unsigned char c)
{
    struct style *s = &in->style;

    s->is_apostrophe = is_apostrophe_text(in, c);
    s->is_colour_start = false;

    const char *wanted = wanted_syntax(in, c);

    if (wanted == s->syntax)
        return;
    if (wanted != NULL) {
        put("\033[38;2;", 7);
        put(wanted, strlen(wanted));
        put("m", 1);
    } else if (s->tint != NULL) {
        put("\033[38;2;", 7);
        put(s->tint, strlen(s->tint));
        put("m", 1);
    } else {
        put_program_colour(s);
    }
    s->syntax = wanted;
}

static void interpret(struct interpreter *in, const unsigned char *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = data[i];
        if (in->state == GROUND && (c < 0x20 || c == 0x7f) && in->screen.word_length > 0)
            end_word(&in->screen);
        if (in->state == GROUND && c >= 0x20 && c != 0x7f && (c < 0x80 || c >= 0xc0)) {
            settle_syntax(in, c);
            settle_ground(in);
        }
        put(&data[i], 1);
        track(in, data[i]);
    }
    flush_output();
}

static void on_signal(int number)
{
    switch (number) {
    case SIGCHLD: child_exited = 1; break;
    case SIGWINCH: window_resized = 1; break;
    default: quit_signal = number; break;
    }
}

static void copy_window_size(int master)
{
    struct winsize size;

    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &size) == 0) {
        ioctl(master, TIOCSWINSZ, &size);
        set_map_size(&interpreter.screen, size.ws_row, size.ws_col);
    }
}

static int open_master(void)
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);

    if (master >= 0 && (grantpt(master) != 0 || unlockpt(master) != 0)) {
        close(master);
        return -1;
    }
    return master;
}

/* Runs in the forked child. Does not return. */
static void become_program(int master, char **program, const struct termios *modes,
                           const sigset_t *original_mask)
{
    const char *slave_name = ptsname(master);
    int slave = -1;

    setsid();
    if (slave_name != NULL)
        slave = open(slave_name, O_RDWR);
    if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) != 0)
        _exit(127);

    tcsetattr(slave, TCSANOW, modes);
    copy_window_size(slave);
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
        close(slave);

    int reset[] = {SIGCHLD, SIGWINCH, SIGHUP, SIGTERM, SIGPIPE};
    for (size_t i = 0; i < sizeof reset / sizeof reset[0]; ++i)
        signal(reset[i], SIG_DFL);
    sigprocmask(SIG_SETMASK, original_mask, NULL);

    execvp(program[0], program);
    perror(program[0]);
    _exit(127);
}

static void pass_input(int master, const unsigned char *input, size_t *start, size_t *end)
{
    ssize_t written = write(master, input + *start, *end - *start);

    if (written > 0)
        *start += (size_t)written;
    if (*start == *end)
        *start = *end = 0;
}

static void relay(int master)
{
    static unsigned char input[BLOCK_SIZE], printed[BLOCK_SIZE];
    struct interpreter *in = &interpreter;
    size_t input_start = 0, input_end = 0;
    bool is_input_open = true;
    sigset_t during_poll;

    sigemptyset(&during_poll);
    copy_window_size(master);

    while (quit_signal == 0 && !terminal_gone) {
        if (window_resized) {
            window_resized = 0;
            copy_window_size(master);
        }

        if (child_exited) {
            ssize_t count;
            while ((count = read(master, printed, sizeof printed)) > 0)
                interpret(in, printed, (size_t)count);
            return;
        }

        bool has_input = input_start < input_end;
        struct pollfd fds[] = {
            {.fd = is_input_open && !has_input ? STDIN_FILENO : -1, .events = POLLIN},
            {.fd = master, .events = (short)(POLLIN | (has_input ? POLLOUT : 0))},
        };

        if (ppoll(fds, 2, NULL, &during_poll) < 0) {
            if (errno == EINTR)
                continue;
            return;
        }

        if (fds[1].revents & POLLIN) {
            ssize_t count = read(master, printed, sizeof printed);
            if (count > 0)
                interpret(in, printed, (size_t)count);
            else if (count == 0 || errno != EAGAIN)
                return;
        } else if (fds[1].revents & (POLLHUP | POLLERR)) {
            return;
        }

        if (fds[1].revents & POLLOUT)
            pass_input(master, input, &input_start, &input_end);

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t count = read(STDIN_FILENO, input, sizeof input);
            if (count > 0)
                input_end = (size_t)count;
            else if (count == 0 || errno != EINTR)
                is_input_open = false;
        }
    }
}

static int run_program(char **program)
{
    struct termios modes;
    sigset_t watched, original_mask;
    int master = -1;

    /* With no terminal around it, or no pseudo-terminal to be had, run plainly. */
    if (tcgetattr(STDIN_FILENO, &modes) != 0 || !isatty(STDOUT_FILENO) ||
        (master = open_master()) < 0) {
        execvp(program[0], program);
        perror(program[0]);
        return 127;
    }
    program_master = master;

    sigemptyset(&watched);
    sigaddset(&watched, SIGCHLD);
    sigaddset(&watched, SIGWINCH);
    sigaddset(&watched, SIGHUP);
    sigaddset(&watched, SIGTERM);
    sigprocmask(SIG_BLOCK, &watched, &original_mask);

    struct sigaction action = {.sa_handler = on_signal, .sa_flags = SA_NOCLDSTOP};
    sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, NULL);
    sigaction(SIGWINCH, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    pid_t child = fork();

    if (child < 0) {
        sigprocmask(SIG_SETMASK, &original_mask, NULL);
        execvp(program[0], program);
        perror(program[0]);
        return 127;
    }
    if (child == 0)
        become_program(master, program, &modes, &original_mask);

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    struct termios raw = modes;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    relay(master);

    tcsetattr(STDIN_FILENO, TCSANOW, &modes);
    close(master);

    if (quit_signal != 0)
        return 128 + quit_signal;

    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* --filter: standard input to standard output, without a program around it. */
static int run_filter(size_t chunk, int rows, int columns)
{
    static unsigned char printed[BLOCK_SIZE];
    ssize_t count;

    if (chunk == 0 || chunk > sizeof printed)
        chunk = sizeof printed;

    set_map_size(&interpreter.screen, rows, columns);
    while ((count = read(STDIN_FILENO, printed, chunk)) > 0)
        interpret(&interpreter, printed, (size_t)count);

    return count < 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "C.UTF-8");

    if (argc > 1 && strcmp(argv[1], "--filter") == 0)
        return run_filter(argc > 2 ? strtoul(argv[2], NULL, 10) : BLOCK_SIZE,
                          argc > 4 ? atoi(argv[3]) : 0, argc > 4 ? atoi(argv[4]) : 0);

    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "usage: berkeDark [program [arguments]]\n");
        return 2;
    }

    const char *shell = getenv("SHELL");
    char *shell_command[] = {(char *)(shell != NULL && *shell != '\0' ? shell : "/bin/sh"), NULL};

    return run_program(argc > 1 ? argv + 1 : shell_command);
}
