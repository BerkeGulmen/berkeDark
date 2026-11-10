/*
 * berkeDark's colours for text styles, as "red;green;blue" in decimal.
 * Text a program coloured itself keeps its own colour.
 */
#define ITALIC_COLOUR "@{prose.emphasis}"       /* @{prose.emphasis|#}, berkeDark's gold */
#define BOLD_ITALIC_COLOUR "@{prose.strong-emphasis}" /* @{prose.strong-emphasis|#} */
#define HEADING_COLOUR "@{prose.heading.1}"       /* @{prose.heading.1|#}: bold, italic and underlined */
#define STRIKE_COLOUR "@{prose.strike}"      /* @{prose.strike|#} */

/*
 * The row behind what the user typed in Claude Code, and the box around such rows.
 * A terminal has no cell borders, so the box is drawn with thin block characters.
 */
#define BAND_BACKGROUND "48;2;@{bg:chat.user.message}" /* @{bg:chat.user.message|#} */
#define BOX_COLOUR "38;2;@{chat.user.message.border}"       /* @{chat.user.message.border|#} */

/*
 * Claude Code's prompt mark at the start of the input row and of the user's rows (the author, 2026-11-08):
 * white, what foot's bold makes of cream text. Bold itself does not show on this mark: the font draws it
 * the same in bold. The input row keeps the program's mark; the rows already sent get a solid one.
 */
#define PROMPT_MARK_COLOUR "@{chat.user.input.mark}" /* @{chat.user.input.mark|#}, foot's bright7 */
#define INPUT_MARK "@{mark:chat.user.input.mark}"              /* the input row: the program's own mark */
#define SENT_MARK "@{mark:chat.user.message.mark}"               /* the user's sent rows */

/*
 * The text typed into Claude Code's input box (the author, 2026-11-10). The program gives it no colour,
 * so it looked like the reply text. The hint of an empty box stays dim, and shell mode (the "!" mark)
 * keeps the program's look. The author also liked #00ff00.
 */
#define INPUT_TEXT_COLOUR "@{chat.user.input.text}" /* @{chat.user.input.text|#}, berkeDark's bright green */

/*
 * Numbers, dates and times standing alone in Claude Code's reply text (the author, 2026-11-08).
 * Numbers were #e87fe0, a soft magenta, until the author chose berkeDark's own number colour (2026-11-10).
 * That soft magenta then went to dates and times, which had shared lavender with inline code (the pool, 2026-11-10).
 */
#define REPLY_NUMBER_COLOUR "@{prose.number}"   /* @{prose.number|#}, berkeDark's number colour */
#define REPLY_DATE_COLOUR "@{prose.date}"   /* @{prose.date|#}, the soft magenta, for dates and times */

/*
 * Reply text in Claude Code (berkeDark's pool, judged by the author on 2026-11-10). The program draws all of
 * these in its text colour; berkeDark finds them on the rows of a reply and writes them again when a frame ends.
 */
#define HEADING_LINE_COLOUR "@{prose.heading.line}"    /* @{prose.heading.line|#} flame, bold: a row that is bold from end to end */
#define LABEL_COLOUR "@{prose.label}"        /* @{prose.label|#} pink, bold: bold words that open a row or a list item and end with a colon */
#define LINK_URL_COLOUR "@{prose.link.url}"      /* @{prose.link.url|#} sky: an address in brackets right after words */
#define LINK_BARE_COLOUR "@{prose.link.bare}"    /* @{prose.link.bare|#} turquoise: an address standing by itself */
#define LIST_MARK_COLOUR "@{prose.list.bullet}"        /* @{prose.list.bullet|#} red: the hyphen or the number before a list item */
#define TASK_OPEN_COLOUR "@{prose.task.open}"     /* @{prose.task.open|#} orange: the mark of a task not yet done */
#define TASK_DONE_COLOUR "@{prose.task.done}"    /* @{prose.task.done|#} green: the mark of a finished task */
#define TASK_OPEN_MARK "@{mark:prose.task.open}"           /* in place of the box "[ ]" */
#define TASK_DONE_MARK "@{mark:prose.task.done}"           /* in place of the box "[x]" */
#define TABLE_HEADER_COLOUR "@{prose.table.header}" /* @{prose.table.header|#} ice, bold: the names of a table's columns */
#define TABLE_BORDER_COLOUR "@{prose.table.border}" /* @{prose.table.border|#}: the lines of a table */
#define RULE_COLOUR "@{prose.rule}"         /* @{prose.rule|#}: a rule between parts of a text */
#define RULE_MARK "@{mark:prose.rule}"                /* the rule is drawn as a line; the program writes three hyphens */
#define QUOTE_TEXT_COLOUR "@{prose.quote.text}"   /* @{prose.quote.text|#} light pink: the italic text beside a quote bar */

/*
 * The mark that starts a reply's text: Claude Code writes it in the theme's text colour, #6fdcf0 in berkeDark's
 * theme, as these arguments after 38. The rows under it are reply text; a tool's mark has another colour.
 */
#define REPLY_MARK_FOREGROUND ";2;@{chat.assistant.mark}"

/*
 * Code in a reply (berkeDark's pool, 2026-11-10). Claude Code draws inline code, and a block of code with no
 * language, in its bright blue (94). In a block with a language its highlighter gives the words a few palette
 * colours, with a role table of its own: keyword, literal and the name of a class blue (34), the name of a
 * function yellow (33), a built-in name and a key cyan (36), a type dim cyan, a string red (31), a number and a
 * comment green (32), a decorator and a preprocessor's row grey (90). berkeDark reads each run with its text
 * on the rows of a reply and writes it again in berkeDark's colour for what it is, when a frame ends.
 */
#define CODE_INLINE_COLOUR "@{prose.code.inline}"      /* @{prose.code.inline|#} lavender: inline code, and a block with no language */
#define PATH_COLOUR "@{prose.path}"              /* @{prose.path|#} gold: a word of inline code that is the path of a file */
/*
 * foot draws bold text of a regular palette colour in the bright one (bold-text-in-bright=palette-based), and
 * lavender and gold are palette colours: bold lavender would turn magenta, bold gold yellow. Bold cells get a
 * colour one step off, which is no palette colour and looks the same.
 */
#define CODE_INLINE_BOLD_COLOUR "@{prose.code.inline|rgb-}"
#define PATH_BOLD_COLOUR "@{prose.path|rgb-}"
#define CODE_KEYWORD_COLOUR "@{code.keyword}"      /* @{code.keyword|#} */
#define CODE_CONSTANT_COLOUR "@{code.constant.language}"    /* @{code.constant.language|#}: true, false, null and their kin */
#define CODE_CLASS_COLOUR "@{code.class}"        /* @{code.class|#}: the name after class, struct, interface, enum */
#define CODE_FUNCTION_COLOUR "@{code.function.definition}"     /* @{code.function.definition|#}: the name of a function where it is defined */
#define CODE_TYPE_COLOUR "@{code.type}"         /* @{code.type|#}: a type; also a built-in name that is not called, a shell command */
#define CODE_CALL_COLOUR "@{code.function.call}"         /* @{code.function.call|#}: a built-in function that is called */
#define CODE_KEY_COLOUR "@{code.property.key}"          /* @{code.property.key|#}: a key in JSON or YAML */
#define CODE_STRING_COLOUR "@{code.string}"        /* @{code.string|#} */
#define CODE_NUMBER_COLOUR "@{code.number}"        /* @{code.number|#} */
#define CODE_COMMENT_COLOUR "@{code.comment}"     /* @{code.comment|#}, italic */
#define CODE_DECORATOR_COLOUR "@{code.decorator}"    /* @{code.decorator|#}, italic */
#define CODE_MACRO_COLOUR "@{code.macro}"        /* @{code.macro|#}: the own words of a preprocessor's row */
#define CODE_PUNCTUATION_COLOUR "@{code.punctuation}" /* @{code.punctuation|#}: the signs of plain code text, as on a diff's rows */

/*
 * A command line (berkeDark's pool, 2026-11-10): what is typed in shell mode, the command a tool is running,
 * the command in a tool's heading. Its parts are read from the text: the name of each command, options,
 * quoted text, variables, paths, numbers and the signs that join commands.
 */
#define COMMAND_NAME_COLOUR "@{command.name}"     /* @{command.name|#} */
#define COMMAND_OPTION_COLOUR "@{command.option}"   /* @{command.option|#}, italic */
#define COMMAND_VARIABLE_COLOUR "@{command.variable}" /* @{command.variable|#} */
#define COMMAND_OPERATOR_COLOUR "@{command.operator}" /* @{command.operator|#} */
#define COMMAND_TEXT_COLOUR "@{command.argument}"    /* @{command.argument|#}: a plain word, where the program drew the line grey */
#define SUGGESTION_COLOUR "@{chat.user.input.suggestion}"        /* @{chat.user.input.suggestion|#}: the dim words offered in the input box, the typed colour at half strength */

/*
 * The grounds of a diff's rows. Claude Code's theme names them (diffAdded, diffRemoved and the two for changed
 * words), but with the dark-ansi base it draws added and removed rows on one grey (235) and the changed words
 * on two more (238 added, 237 removed). berkeDark writes the theme's four grounds in their place, as arguments
 * of a style mark: the row's kind is read from the mark in its gutter.
 */
#define DIFF_ADDED_GROUND "48;2;@{bg:diff.added}"        /* @{bg:diff.added|#} */
#define DIFF_ADDED_WORD_GROUND "48;2;@{bg:diff.added.word}"   /* @{bg:diff.added.word|#} */
#define DIFF_REMOVED_GROUND "48;2;@{bg:diff.removed}"      /* @{bg:diff.removed|#} */
#define DIFF_REMOVED_WORD_GROUND "48;2;@{bg:diff.removed.word}" /* @{bg:diff.removed.word|#} */

/*
 * Claude Code's diff asks for its syntax colours by palette name, with a role table of its own:
 * keywords bright magenta, numbers bright blue, strings bright green, function and class names
 * bright yellow, plain code text white (37). On a code row of a diff, one that starts with a line
 * number or a + / - mark, berkeDark gives these roles berkeDark's colours, and the punctuation of
 * plain code text a colour of its own (the author, 2026-11-08). Dim rows (removed lines) and
 * everything outside the code keep theirs.
 */
#define SYNTAX_KEYWORD_COLOUR "@{code.keyword}"      /* @{code.keyword|#}, for the program's bright magenta (95) */
#define SYNTAX_NUMBER_COLOUR "@{code.number}"        /* @{code.number|#}, for its bright blue (94) */
#define SYNTAX_STRING_COLOUR "@{code.string}"        /* @{code.string|#}, for its bright green (92) */
#define SYNTAX_FUNCTION_COLOUR "@{code.function.definition}"     /* @{code.function.definition|#}, berkeDark's function colour, for its bright yellow (93) */
#define SYNTAX_PUNCTUATION_COLOUR "@{code.punctuation}" /* @{code.punctuation|#}, punctuation in plain code text (37) */
#define SYNTAX_PUNCTUATION "()[]{}:;,./\\'\"`"

/*
 * A diff's rows are coloured one by one, so inside a comment of several rows an apostrophe looks like the
 * start of a string and the words after it arrive in the string colour (the author's observation, 2026-11-10).
 * A string never opens right after a word, so such a run is written as plain code text.
 */
#define SYNTAX_TEXT_COLOUR "@{code.text}" /* @{code.text|#}, the palette's white: plain code text */
