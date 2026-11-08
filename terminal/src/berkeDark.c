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

enum { CELL_USED = 1, CELL_BAND = 2, CELL_POINTER = 4, CELL_TOOL = 8, CELL_PLAIN = 16, CELL_BOLD = 32, CELL_PAINTED = 64 };

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
};

struct screen {
    bool is_alternate;
    bool is_claude;  /* the full-screen program is Claude Code: its marks and diff colours apply */
    bool has_saved_cursor, is_insert, is_origin; /* the program's states in which numbers are left alone */
    bool has_band_changed;
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
    } else if (wanted == NULL && s->tint != NULL) {
        put("\033[39m", 5);
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
            break;
        case 48:
            s->background = true;
            s->band = strncmp(start, BAND_BACKGROUND, strlen(BAND_BACKGROUND)) == 0;
            if (!has_subparameter)
                p = skip_colour_arguments(p);
            break;
        case 49:
            s->background = s->band = false;
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
    int amount = abs(count) < height ? abs(count) : height;
    size_t kept = (size_t)(height - amount);

    if (count > 0) {
        memmove(sc->cells[top], sc->cells[top + amount], kept * sizeof sc->cells[0]);
        memmove(sc->text[top], sc->text[top + amount], kept * sizeof sc->text[0]);
        memmove(&sc->edges[top], &sc->edges[top + amount], kept);
        memmove(&sc->code_from[top], &sc->code_from[top + amount], kept * sizeof sc->code_from[0]);
        memset(sc->cells[bottom - amount + 1], 0, (size_t)amount * sizeof sc->cells[0]);
        memset(sc->text[bottom - amount + 1], 0, (size_t)amount * sizeof sc->text[0]);
        memset(&sc->edges[bottom - amount + 1], NO_EDGE, (size_t)amount);
        memset(&sc->code_from[bottom - amount + 1], 0xff, (size_t)amount * sizeof sc->code_from[0]);
    } else {
        memmove(sc->cells[top + amount], sc->cells[top], kept * sizeof sc->cells[0]);
        memmove(sc->text[top + amount], sc->text[top], kept * sizeof sc->text[0]);
        memmove(&sc->edges[top + amount], &sc->edges[top], kept);
        memmove(&sc->code_from[top + amount], &sc->code_from[top], kept * sizeof sc->code_from[0]);
        memset(sc->cells[top], 0, (size_t)amount * sizeof sc->cells[0]);
        memset(sc->text[top], 0, (size_t)amount * sizeof sc->text[0]);
        memset(&sc->edges[top], NO_EDGE, (size_t)amount);
        memset(&sc->code_from[top], 0xff, (size_t)amount * sizeof sc->code_from[0]);
    }
    sc->run_kind = 0;
    sc->run_length = 0;
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

static bool may_start_code(const struct screen *sc)
{
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
        end_gutter_run(sc);
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

static bool is_separator(unsigned code_point)
{
    return code_point == ' ' || code_point == 0xa0 || code_point == 0x2502;
}

/* Follows the run being printed; a separator or a gap ends it (and so does any escape, in interpret). */
static void follow_word(struct screen *sc, unsigned code_point, int width)
{
    if (is_separator(code_point)) {
        end_word(sc);
        return;
    }
    if (sc->word_length > 0 && (sc->word_row != sc->row || sc->word_column + sc->word_length != sc->column))
        end_word(sc);
    if (sc->word_length == 0) {
        sc->word_row = sc->row;
        sc->word_column = sc->column;
    }
    sc->word_length += width;
}

static void print_code_point(struct interpreter *in, unsigned code_point)
{
    struct screen *sc = &in->screen;
    int width = code_point < 0x80 ? 1 : wcwidth((wchar_t)code_point);

    if (width == 0)
        return;
    if (width < 0)
        width = 1;
    if (sc->column + width > sc->columns) {
        sc->word_length = 0;
        sc->column = 0;
        line_feed(sc);
    }
    follow_gutter(in, code_point);
    follow_word(sc, code_point, width);

    unsigned char flags = blank_flags(&in->style);

    if (code_point != ' ')
        flags |= CELL_USED;
    if (code_point == 0x23bf)
        flags |= CELL_TOOL;
    bool is_prompt_mark = code_point == 0x276f && sc->column == 0;
    if (is_prompt_mark && !in->style.band)
        flags |= CELL_POINTER;

    const struct style *s = &in->style;
    bool is_word_character = !is_separator(code_point);

    if (is_word_character && width == 1 && s->foreground == 0 && !s->background && !s->dim && !s->italic &&
        !s->underline && !s->strike && !s->reverse && s->tint == NULL)
        flags |= s->bold ? CELL_PLAIN | CELL_BOLD : CELL_PLAIN;
    for (int i = 0; i < width; ++i) {
        set_cell(sc, sc->row, sc->column, flags);
        sc->text[sc->row][sc->column++] = !is_word_character ? 0 : code_point < 0x80 ? (unsigned char)code_point : 0x80;
    }
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
    unsigned char *line = sc->cells[sc->row], *text = sc->text[sc->row];
    int room = sc->columns - sc->column;
    int amount = abs(count) < room ? abs(count) : room;

    if (count > 0) {
        memmove(line + sc->column + amount, line + sc->column, (size_t)(room - amount));
        memmove(text + sc->column + amount, text + sc->column, (size_t)(room - amount));
        clear_cells(in, sc->row, sc->column, sc->column + amount);
    } else {
        memmove(line + sc->column, line + sc->column + amount, (size_t)(room - amount));
        memmove(text + sc->column, text + sc->column + amount, (size_t)(room - amount));
        clear_cells(in, sc->row, sc->columns - amount, sc->columns);
    }
}

static void follow_cursor_sequence(struct interpreter *in, unsigned char final)
{
    struct screen *sc = &in->screen;
    int values[2] = {0, 0}, count = 0;

    for (const char *p = in->parameters; count < 2; ++count) {
        values[count] = atoi(p);
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
    case 'd': sc->row = amount - 1; break;
    case 'H':
    case 'f':
        sc->row = amount - 1;
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
    case 'L': scroll_rows(sc, sc->row, sc->bottom, -amount); break;
    case 'M': scroll_rows(sc, sc->row, sc->bottom, amount); break;
    case 'S': scroll_rows(sc, sc->top, sc->bottom, amount); break;
    case 'T': scroll_rows(sc, sc->top, sc->bottom, -amount); break;
    case 'r':
        sc->top = amount - 1;
        sc->bottom = (values[1] > 0 ? values[1] : sc->rows) - 1;
        if (sc->top < 0 || sc->bottom >= sc->rows || sc->top >= sc->bottom) {
            sc->top = 0;
            sc->bottom = sc->rows - 1;
        }
        sc->row = sc->column = 0;
        break;
    case 's':
        sc->saved_row = sc->row;
        sc->saved_column = sc->column;
        sc->has_saved_cursor = true;
        break;
    case 'u':
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

    int n = to - from, number_from = 0, number_to = 0;
    const char *t = (const char *)&sc->text[row][from];

    if (from <= 2 && is_tool_word(t, n))
        sc->cells[row][from] |= CELL_TOOL;

    bool is_number = n <= 48, is_date = false;
    unsigned char first = sc->cells[row][from];

    for (int column = from; column < to; ++column)
        if ((sc->cells[row][column] & CELL_PLAIN) == 0 || ((sc->cells[row][column] ^ first) & CELL_BOLD) != 0)
            is_number = false;
    is_number = is_number && find_number(t, n, &number_from, &number_to, &is_date) && !is_interface_row(sc, row);
    if (is_number && from <= 5 && number_from == 0 && number_to == n - 1 && t[n - 1] == '.' &&
        skip_digits(t, n, 0) == number_to)
        is_number = false; /* the number of a list or a menu */

    int paint_from = is_number ? from + number_from : to, paint_to = is_number ? from + number_to : to;
    bool has_work = is_number;

    for (int column = from; column < to; ++column)
        if ((column < paint_from || column >= paint_to) && (sc->cells[row][column] & CELL_PAINTED) != 0)
            has_work = true;
    if (!has_work)
        return;

    put_text("\0337");
    for (int column = from; column < to; ++column) {
        if ((column < paint_from || column >= paint_to) && (sc->cells[row][column] & CELL_PAINTED) != 0) {
            put_move(row, column);
            put_text((sc->cells[row][column] & CELL_BOLD) != 0 ? "\033[0;1m" : "\033[0m");
            put(&sc->text[row][column], 1);
            sc->cells[row][column] &= (unsigned char)~CELL_PAINTED;
        }
    }
    if (is_number) {
        put_move(row, paint_from);
        put_text((first & CELL_BOLD) != 0 ? "\033[0;1;38;2;" : "\033[0;38;2;");
        put_text(is_date ? REPLY_DATE_COLOUR "m" : REPLY_NUMBER_COLOUR "m");
        put(&sc->text[row][paint_from], (size_t)(paint_to - paint_from));
        for (int column = paint_from; column < paint_to; ++column)
            sc->cells[row][column] |= CELL_PAINTED;
    }
    put_text("\0338");
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
    if (!has_work)
        return;

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
        int mode = atoi(p);

        if (mode == 1049 || mode == 1047 || mode == 47) {
            sc->is_alternate = is_on;
            sc->is_claude = is_on && foreground_is_claude();
            clear_map(sc);
        } else if (mode == 6) {
            sc->is_origin = is_on;
        } else if (mode == 2026 && !is_on && has_map(sc)) {
            draw_box(in);
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

    if (final == 'm')
        apply_style_sequence(in);
    else if (final == 's' && in->length == 0)
        in->saved_style = in->style;
    else if (final == 'u' && in->length == 0)
        in->style = in->saved_style;
    else if (has_map(&in->screen))
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

static const char *wanted_syntax(const struct interpreter *in, unsigned char c)
{
    const struct style *s = &in->style;
    const struct screen *sc = &in->screen;

    if (!has_map(sc) || !sc->is_claude || s->dim || sc->row >= sc->rows || sc->code_from[sc->row] < 0 ||
        sc->column < sc->code_from[sc->row])
        return NULL;
    switch (s->foreground) {
    case 95: return SYNTAX_KEYWORD_COLOUR;
    case 94: return SYNTAX_NUMBER_COLOUR;
    case 92: return SYNTAX_STRING_COLOUR;
    case 93: return SYNTAX_FUNCTION_COLOUR;
    case 37: return c < 0x80 && strchr(SYNTAX_PUNCTUATION, c) != NULL ? SYNTAX_PUNCTUATION_COLOUR : NULL;
    default: return NULL;
    }
}

static void settle_syntax(struct interpreter *in, unsigned char c)
{
    struct style *s = &in->style;
    const char *wanted = wanted_syntax(in, c);

    if (wanted == s->syntax)
        return;
    if (wanted != NULL) {
        put("\033[38;2;", 7);
        put(wanted, strlen(wanted));
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
        if (in->state == GROUND && c >= 0x20 && c != 0x7f && (c < 0x80 || c >= 0xc0))
            settle_syntax(in, c);
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
