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
