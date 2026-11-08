/*
 * berkeDark's colours for text styles, as "red;green;blue" in decimal.
 * Text a program coloured itself keeps its own colour.
 */
#define ITALIC_COLOUR "255;210;92"       /* #ffd25c, berkeDark's gold */
#define BOLD_ITALIC_COLOUR "255;249;196" /* #fff9c4 */
#define HEADING_COLOUR "255;255;0"       /* #ffff00: bold, italic and underlined */
#define STRIKE_COLOUR "245;235;208"      /* #f5ebd0 */

/*
 * The row behind what the user typed in Claude Code, and the box around such rows.
 * A terminal has no cell borders, so the box is drawn with thin block characters.
 */
#define BAND_BACKGROUND "48;2;38;3;7" /* #260307 */
#define BOX_COLOUR "38;2;0;0;0"       /* #000000 */

/*
 * Claude Code's prompt mark at the start of the input row and of the user's rows (the author, 2026-11-08):
 * white, what foot's bold makes of cream text. Bold itself does not show on this mark: the font draws it
 * the same in bold. The input row keeps the program's mark; the rows already sent get a solid one.
 */
#define PROMPT_MARK_COLOUR "250;252;255" /* #fafcff, foot's bright7 */
#define INPUT_MARK "\u276f"              /* the input row: the program's own mark */
#define SENT_MARK "\u25b6"               /* the user's sent rows */

/* Numbers, dates and times standing alone in Claude Code's reply text (the author, 2026-11-08). */
#define REPLY_NUMBER_COLOUR "232;127;224" /* #e87fe0, a soft magenta */
#define REPLY_DATE_COLOUR "163;165;245"   /* #a3a5f5, berkeDark's lavender, for dates and times */

/*
 * Claude Code's diff asks for its syntax colours by palette name, with a role table of its own:
 * keywords bright magenta, numbers bright blue, strings bright green, function and class names
 * bright yellow, plain code text white (37). On a code row of a diff, one that starts with a line
 * number or a + / - mark, berkeDark gives these roles berkeDark's colours, and the punctuation of
 * plain code text a colour of its own (the author, 2026-11-08). Dim rows (removed lines) and
 * everything outside the code keep theirs.
 */
#define SYNTAX_KEYWORD_COLOUR "87;191;255"      /* #57bfff, for the program's bright magenta (95) */
#define SYNTAX_NUMBER_COLOUR "255;0;255"        /* #ff00ff, for its bright blue (94) */
#define SYNTAX_STRING_COLOUR "255;255;0"        /* #ffff00, for its bright green (92) */
#define SYNTAX_FUNCTION_COLOUR "63;127;255"     /* #3f7fff, berkeDark's function colour, for its bright yellow (93) */
#define SYNTAX_PUNCTUATION_COLOUR "178;217;255" /* #b2d9ff, punctuation in plain code text (37) */
#define SYNTAX_PUNCTUATION "()[]{}:;,./\\'\"`"
