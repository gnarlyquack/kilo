//
// includes
//

#include <assert.h> // assert
#include <ctype.h> // iscntrl
#include <errno.h> // errno, EAGAIN
#include <fcntl.h> // open, O_RDONLY
#include <limits.h> // UCHAR_MAX
#include <poll.h> // struct pollfd, nfds_t, poll, POLLIN
#include <stdarg.h>
#include <stdio.h> // perror, printf, snprintf
#include <stdlib.h> // atexit, exit, free, malloc, realloc, EXIT_SUCCESS
#include <string.h> // memcpy, strlen, strstr
#include <sys/ioctl.h> // struct winsize, ioctl, TIOCGWINSZ
#include <termios.h> // struct termios, tcgetattr, tcsetattr, BRKINT, CS8, ECHO, ICANON,
                     // ICRNL, IEXTEN, INPCK, ISIG, ISTRIP, IXON, OPOST, TCSAFLUSH, VMIN,
                     // VTIME
#include <time.h> // time_t
#include <unistd.h> // read, write, STDIN_FILENO, STDOUT_FILENO


//
// defines
//

#define KILO_VERSION "0.0.1"

#define KILO_TAB_STOP 8

#define ARRAYLEN(array) (sizeof(array)/sizeof((array)[0]))

#define CAST(type) (type)
#define CTRL_KEY(key) ((key) & 0x1f)


typedef enum EditorKey
{
    BACKSPACE = 127,
    ARROW_LEFT = UCHAR_MAX + 1,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    END_KEY,
    HOME_KEY,
    PAGE_DOWN,
    PAGE_UP,
} EditorKey;


//
// data
//

typedef struct Buffer
{
    unsigned len;
    char *b;
} Buffer;


typedef struct Line
{
    Buffer raw;
    Buffer render;
} Line;


typedef struct Editor
{
    unsigned screenrows;
    unsigned screencols;
    unsigned cx, cy;
    unsigned rx;
    unsigned rowoff;
    unsigned coloff;
    unsigned numlines;
    unsigned dirty;
    Line *lines;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
} Editor;

static Editor editor;


//
// prototypes
//

static void editorSetStatusMessage(const char *fmt, ...);

static void editorRefreshScreen(void);

typedef void PromptCallback(char *, int);
static char *editorPrompt(char *prompt, PromptCallback callback);


//
// terminal
//

static _Noreturn void
die(const char *message)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(message);
    exit(EXIT_FAILURE);
}

static void
disableRawMode(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &editor.orig_termios);
}


static void
enableRawMode(void)
{
    if (-1 == tcgetattr(STDIN_FILENO, &editor.orig_termios))
    {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = editor.orig_termios;
    raw.c_cflag |= CS8;
    raw.c_iflag &= ~(CAST(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_lflag &= ~(CAST(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_oflag &= ~(CAST(tcflag_t)OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw))
    {
        die("tcsetattr");
    }
}


static int
terminalHasKey(int timeout)
{
    nfds_t nfds = 1;
    struct pollfd fd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };
    int result = poll(&fd, nfds, timeout);
    return result;
}


static char
terminalNextKey(int timeout)
{
    char c = 0;
    if (terminalHasKey(timeout))
    {
        ssize_t nread = read(STDIN_FILENO, &c, 1);
        if ((-1 == nread) && (EAGAIN != errno))
        {
            die("read");
        }
    }
    return c;
}



static int
terminalReadKey(void)
{
    int c = terminalNextKey(-1);
    if ('\x1b' == c)
    {
        switch (terminalNextKey(0))
        {
            case 0:
            {
                // Just return the escape character
            } break;

            case '[':
            {
                char next = terminalNextKey(0);
                if ((next >= '0' && next <= '9'))
                {
                    if ('~' == terminalNextKey(0))
                    {
                        switch (next)
                        {
                            case '1':
                            {
                                c = HOME_KEY;
                            } break;

                            case '3':
                            {
                                c = DEL_KEY;
                            } break;

                            case '4':
                            {
                                c = END_KEY;
                            } break;

                            case '5':
                            {
                                c = PAGE_UP;
                            } break;

                            case '6':
                            {
                                c = PAGE_DOWN;
                            } break;

                            case '7':
                            {
                                c = HOME_KEY;
                            } break;

                            case '8':
                            {
                                c = END_KEY;
                            } break;

                            default:
                            {
                                // Unhandled escape code
                                c = 0;
                            } break;
                        }
                    }
                    else
                    {
                        // Unhandled escape code
                        c = 0;
                    }
                }
                else
                {
                    switch (next)
                    {
                        case 'A':
                        {
                            c = ARROW_UP;
                        } break;

                        case 'B':
                        {
                            c = ARROW_DOWN;
                        } break;

                        case 'C':
                        {
                            c = ARROW_RIGHT;
                        } break;

                        case 'D':
                        {
                            c = ARROW_LEFT;
                        } break;

                        case 'F':
                        {
                            c = END_KEY;
                        } break;

                        case 'H':
                        {
                            c = HOME_KEY;
                        } break;

                        default:
                        {
                            // Unhandled escape code
                            c = 0;
                        } break;
                    }
                }
            } break;

            case 'O':
            {
                switch (terminalNextKey(0))
                {
                    case 'F':
                    {
                        c = END_KEY;
                    } break;

                    case 'H':
                    {
                        c = HOME_KEY;
                    } break;

                    default:
                    {
                        // Unhandled escape code
                        c = 0;
                    } break;
                }
            } break;

            default:
            {
                // Unhandled escape code
                c = 0;
            } break;
        }
    }

    return c;
}


static int
getWindowSize(unsigned *rows, unsigned *cols)
{
    int result = 0;
    struct winsize ws;

    if (((-1 == ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) && (0 != errno))
        || (0 == ws.ws_col))
    {
        result = -1;
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }

    return result;
}


//
// string buffer
//

static void
buffer_init(Buffer *buffer)
{
    buffer->len = 0;
    buffer->b = 0;
}


static void
buffer_append(Buffer *buffer, const char *s, unsigned len)
{
    if (len)
    {
        assert((buffer->len + len) > buffer->len);
        char *new = realloc(buffer->b, buffer->len + len + 1);

        if (new)
        {
            memcpy(new + buffer->len, s, len);
            buffer->b = new;
            buffer->len += len;
            buffer->b[buffer->len] = 0;
        }
    }
}


static void
buffer_free(Buffer *buffer)
{
    free(buffer->b);
    buffer->b = 0;
    buffer->len = 0;
}


//
// row operations
//

static unsigned
editorRowCxToRx(Line *line, unsigned cx)
{
    unsigned rx = 0;
    for (unsigned i = 0; i < cx; ++i)
    {
        if ('\t' == line->raw.b[i])
        {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        ++rx;
    }

    return rx;
}


static unsigned
editorRowRxToCx(Line *line, unsigned rx)
{
    unsigned current_rx = 0;
    unsigned cx;
    for (cx = 0; cx < line->raw.len; ++cx)
    {
        if ('\t' == line->raw.b[cx])
        {
            current_rx += (KILO_TAB_STOP - 1) - (current_rx % KILO_TAB_STOP);
        }
        ++current_rx;

        if (current_rx > rx)
        {
            break;
        }
    }


    return cx;
}


static void
editorUpdateRow(Line *line)
{
    free(line->render.b);
    line->render.b = 0;
    line->render.len = 0;
    unsigned r = 0;

    for (unsigned i = 0; i < line->raw.len; ++i)
    {
        char c = line->raw.b[i];
        if ('\t' == c)
        {
            c = ' ';
            do
            {
                ++r;
                buffer_append(&line->render, &c, 1);
            } while (r % KILO_TAB_STOP);
        }
        else
        {
            ++r;
            buffer_append(&line->render, &c, 1);
        }
    }
}


static void
editorInsertRow(unsigned at, const char *s, unsigned len)
{
    Line *new = realloc(editor.lines, sizeof(*new) * (editor.numlines + 1));
    if (new)
    {
        editor.lines = new;
        for (unsigned i = editor.numlines; i > at; --i)
        {
            editor.lines[i] = editor.lines[i - 1];
        }

        new = editor.lines + at;
        if (len)
        {
            new->raw.b = malloc(len + 1);
            if (new->raw.b)
            {
                memcpy(new->raw.b, s, len);
                new->raw.b[len] = 0;
            }
            else
            {
                die("editorInsertRow");
            }
        }
        else
        {
            new->raw.b = 0;
        }

        new->raw.len = len;
        new->render.len = 0;
        new->render.b = 0;
        ++editor.numlines;
        ++editor.dirty;
        editorUpdateRow(new);
    }
    else
    {
        die("editorInsertRow");
    }
}


static void
editorFreeRow(Line *line)
{
    free(line->raw.b);
    free(line->render.b);
}


static void
editorDelRow(unsigned at)
{
    if (at < editor.numlines)
    {
        editorFreeRow(editor.lines + at);
        --editor.numlines;
        while (at < editor.numlines)
        {
            editor.lines[at] = editor.lines[at + 1];
            ++at;
        }
        ++editor.dirty;
    }
}


static void
editorRowInsertChar(Line *line, unsigned at, int c)
{
    if (at > line->raw.len)
    {
        at = line->raw.len;
    }

    char *new = realloc(line->raw.b, line->raw.len + 1);
    if (new)
    {
        line->raw.b = new;
        for (unsigned i = line->raw.len; i > at; --i)
        {
            line->raw.b[i] = line->raw.b[i - 1];
        }
        ++line->raw.len;
        line->raw.b[at] = CAST(char)c;
        line->raw.b[line->raw.len] = 0;
        ++editor.dirty;
        editorUpdateRow(line);
    }
    else
    {
        die("editorRowInsertChar");
    }
}


static void
editorRowAppendString(Line *line, char *s, unsigned len)
{
    char *new = line->raw.b;
    unsigned size = line->raw.len + len;
    if (size > line->raw.len)
    {
        new = realloc(line->raw.b, size);
        if (new)
        {
            for (size_t i = 0; i < len; ++i)
            {
                new[line->raw.len + i] = s[i];
            }
        }
        else
        {
            die("editorRowAppendString");
        }
    }

    line->raw.b = new;
    line->raw.len = size;
    ++editor.dirty;
    editorUpdateRow(line);
}


static void
editorRowDelChar(Line *line, unsigned at)
{
    if (at < line->raw.len)
    {
        --line->raw.len;
        while (at <= line->raw.len)
        {
            line->raw.b[at] = line->raw.b[at + 1];
            ++at;
        }
        editorUpdateRow(line);
        ++editor.dirty;
    }
}


//
// editor operations
//

static void
editorInsertChar(int c)
{
    if (editor.cy == editor.numlines)
    {
        editorInsertRow(editor.cy, 0, 0);
    }
    editorRowInsertChar(editor.lines + editor.cy, editor.cx, c);
    ++editor.cx;
}


static void
editorInsertNewline(void)
{
    if (editor.cx)
    {
        Line *line = editor.lines + editor.cy;
        editorInsertRow(editor.cy + 1, line->raw.b + editor.cx, line->raw.len - editor.cx);

        // NOTE: editorInsertRow reallocs editor.lines, which means line is now invalid
        line = editor.lines + editor.cy;
        line->raw.len = editor.cx;
        line->raw.b[line->raw.len] = 0;
        editorUpdateRow(line);
        editor.cx = 0;
    }
    else
    {
        editorInsertRow(editor.cy, 0, 0);
    }
    ++editor.cy;
}


static void
editorDelChar(void)
{
    if (editor.cy < editor.numlines)
    {
        Line *line = editor.lines + editor.cy;
        if (editor.cx)
        {
            editorRowDelChar(line, --editor.cx);
        }
        else if (editor.cy)
        {
            --editor.cy;
            editor.cx = editor.lines[editor.cy].raw.len;
            editorRowAppendString(editor.lines + editor.cy, line->raw.b, line->raw.len);
            editorDelRow(editor.cy + 1);
        }
    }
}


//
// file i/o
//

static char *
editorRowsToString(unsigned *buflen)
{
    unsigned totlen = 0;
    for (unsigned j = 0; j < editor.numlines; ++j)
    {
        totlen += editor.lines[j].raw.len + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (unsigned j = 0; j < editor.numlines; ++j)
    {
        memcpy(p, editor.lines[j].raw.b, editor.lines[j].raw.len);
        p += editor.lines[j].raw.len;
        *p = '\n';
        ++p;
    }

    return buf;
}


static void
editorOpen(const char *filename)
{
    free(editor.filename);

    int fd = open(filename, O_RDONLY);
    if (-1 == fd)
    {
        die("open");
    }

    editor.filename = malloc(strlen(filename) + 1);
    if (editor.filename)
    {
        size_t i;
        for (i = 0; i < strlen(filename); ++i)
        {
            editor.filename[i] = filename[i];
        }
        editor.filename[i] = 0;
    }
    else
    {
        die("strdup");
    }

    ssize_t nread;
    do
    {
        Buffer line = {0};
        char c;
        do
        {
            nread = read(fd, &c, 1);
            if (nread < 0)
            {
                die("reading file");
            }
            else if (nread)
            {
                buffer_append(&line, &c, 1);
            }
            else
            {
                break;
            }
        } while ('\n' != c);

        if (line.len)
        {
            --line.len;
            if (line.len && ('\r' == line.b[line.len - 1]))
            {
                --line.len;
            }
            editorInsertRow(editor.numlines, line.b, line.len);
        }
    } while (nread);

    editor.dirty = 0;
}


static void
editorSave(void)
{
    if (!editor.filename)
    {
        editor.filename = editorPrompt("Save as: %s", 0);
        if (!editor.filename)
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int fd = open(editor.filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (-1 == fd)
    {
        editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
    }
    else
    {
        unsigned len;
        char *buf = editorRowsToString(&len);

        while (len)
        {
            ssize_t written = write(fd, buf, len);
            if (-1 == written)
            {
                editorSetStatusMessage("Error while saving: %s", strerror(errno));
                break;
            }
            else
            {
                len -= CAST(unsigned)written;
            }
        }
        close(fd);
        free(buf);
        editor.dirty = len;
    }
}


//
// find
//

static void
editorFindCallback(char *query, int key)
{
    static unsigned last_match = CAST(unsigned)-1;
    static int direction = 1;

    switch (key)
    {
        case '\r':
        case '\x1b':
        {
            last_match = CAST(unsigned)-1;
            direction = 1;
            return;
        } break;

        case ARROW_RIGHT:
        case ARROW_DOWN:
        {
            direction = 1;
        } break;

        case ARROW_LEFT:
        case ARROW_UP:
        {
            direction = -1;
        } break;

        default:
        {
            last_match = CAST(unsigned)-1;
            direction = 1;
        }
    }

    unsigned current = last_match;
    for (unsigned i = 0; i < editor.numlines; ++i)
    {
        current += CAST(unsigned)direction;
        if (current >= editor.numlines)
        {
            if (1 == direction)
            {
                current = 0;
            }
            else
            {
                current = editor.numlines - 1;
            }
        }

        Line *line = editor.lines + current;
        if (line->render.len)
        {
            char *match = strstr(line->render.b, query);
            if (match)
            {
                last_match = current;
                editor.cy = current;
                editor.cx = editorRowRxToCx(line, CAST(unsigned)(match - line->render.b));
                editor.rowoff = editor.numlines;
                break;
            }
        }
    }
}


static void
editorFind(void)
{
    unsigned cx = editor.cx;
    unsigned cy = editor.cy;
    unsigned coloff = editor.coloff;
    unsigned rowoff = editor.rowoff;

    char *query = editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);
    if (query)
    {
        free(query);
    }
    else
    {
        editor.cx = cx;
        editor.cy = cy;
        editor.coloff = coloff;
        editor.rowoff = rowoff;
    }
}


//
// input
//

static char *
editorPrompt(char *prompt, PromptCallback callback)
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = 0;

    for (;;)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = terminalReadKey();
        if ((DEL_KEY == c) || (CTRL_KEY('h') == c) || (BACKSPACE == c))
        {
            if (buflen)
            {
                buf[--buflen] = 0;
            }
        }
        else if ('\x1b' == c)
        {
            editorSetStatusMessage("");
            if (callback)
            {
                callback(buf, c);
            }
            free(buf);
            buf = 0;
            break;
        }
        else if ('\r' == c)
        {
            if (buflen)
            {
                editorSetStatusMessage("");
                if (callback)
                {
                    callback(buf, c);
                }
                break;
            }
        }
        else if ((c > 31) && (c < 127))
        {
            if ((buflen + 1) == bufsize)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = CAST(char)c;
            buf[buflen] = 0;
        }

        if (callback)
        {
            callback(buf, c);
        }
    }

    return buf;
}


static void
editorMoveCursor(int key)
{
    Line *line = 0;
    if (editor.cy < editor.numlines)
    {
        line = editor.lines + editor.cy;
    }

    switch (key)
    {
        case ARROW_LEFT:
        {
            if (editor.cx)
            {
                --editor.cx;
            }
            else if (editor.cy)
            {
                --editor.cy;
                editor.cx = editor.lines[editor.cy].raw.len;
            }
        } break;

        case ARROW_RIGHT:
        {
            if (line)
            {
                if (editor.cx < line->raw.len)
                {
                    ++editor.cx;
                }
                else if ((editor.cy + 1) < editor.numlines)
                {
                    ++editor.cy;
                    editor.cx = 0;
                }
            }
        } break;

        case ARROW_DOWN:
        {
            if ((editor.cy + 1) < editor.numlines)
            {
                ++editor.cy;
            }
        } break;

        case ARROW_UP:
        {
            if (editor.cy)
            {
                --editor.cy;
            }
        } break;
    }

    if (editor.cy < editor.numlines)
    {
        line = editor.lines + editor.cy;
        if (editor.cx > line->raw.len)
        {
            editor.cx = line->raw.len;
        }
    }
    else
    {
        editor.cx = 0;
    }
}


static void
editorProcessKeyPress(void)
{
    static int confirm_quit = 0;
    int c = terminalReadKey();

    if (confirm_quit)
    {
        switch (c)
        {
            case 'y':
            case 'Y':
            {
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                exit(EXIT_SUCCESS);
            } break;

            case 'n':
            case 'N':
            case '\r':
            {
                confirm_quit = 0;
                editorSetStatusMessage("");
            } break;

            default:
            {
                editorSetStatusMessage(
                    "WARNING!!! File has unsaved changes. Are you sure you want to quit? (y/N)");
            } break;
        }
    }
    else
    {
        switch (c)
        {
            case '\r':
            {
                editorInsertNewline();
            } break;

            case CTRL_KEY('f'):
            {
                editorFind();
            } break;

            case CTRL_KEY('q'):
            {
                if (!editor.dirty || confirm_quit)
                {
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    exit(EXIT_SUCCESS);
                }
                else
                {
                    confirm_quit = 1;
                    editorSetStatusMessage(
                        "WARNING!!! File has unsaved changes. Are you sure you want to quit? (y/N)");
                }
            } break;

            case CTRL_KEY('s'):
            {
                editorSave();
            } break;

            case ARROW_UP:
            case ARROW_DOWN:
            case ARROW_LEFT:
            case ARROW_RIGHT:
            {
                editorMoveCursor(c);
            } break;

            case BACKSPACE:
            case CTRL_KEY('h'):
            case DEL_KEY:
            {
                if (DEL_KEY == c)
                {
                    editorMoveCursor(ARROW_RIGHT);
                }
                editorDelChar();
            } break;

            case END_KEY:
            {
                if (editor.cy < editor.numlines)
                {
                    editor.cx = editor.lines[editor.cy].raw.len;
                }
            } break;

            case HOME_KEY:
            {
                editor.cx = 0;
            } break;

            case PAGE_DOWN:
            {
                for (unsigned n = 1; n < editor.screenrows; ++n)
                {
                    editorMoveCursor(ARROW_DOWN);
                }
            } break;

            case PAGE_UP:
            {
                for (unsigned n = 1; n < editor.screenrows; ++n)
                {
                    editorMoveCursor(ARROW_UP);
                }
            } break;

            case CTRL_KEY('l'):
            case '\x1b':
            {
            } break;

            default:
            {
                editorInsertChar(c);
            } break;
        }
    }
}


//
// output
//

void editorScroll(void)
{
    editor.rx = editor.cx;
    if (editor.cy < editor.numlines)
    {
        editor.rx = editorRowCxToRx(editor.lines + editor.cy, editor.cx);
    }

    if (editor.cy < editor.rowoff)
    {
        editor.rowoff = editor.cy;
    }
    else if (editor.cy >= (editor.rowoff + editor.screenrows))
    {
        editor.rowoff = editor.cy - editor.screenrows + 1;
    }

    if (editor.rx < editor.coloff)
    {
        editor.coloff = editor.rx;
    }
    else if (editor.rx >= (editor.coloff + editor.screencols))
    {
        editor.coloff = editor.rx - editor.screencols + 1;
    }
}


static void
editorDrawRows(Buffer *buffer)
{
    for (unsigned y = 0; y < editor.screenrows; ++y)
    {
        unsigned row = y + editor.rowoff;
        if (row >= editor.numlines)
        {
            if (!editor.numlines && (y == (editor.screenrows / 3)))
            {
                char welcome[80];
                int result = snprintf(
                        welcome, sizeof(welcome), "Kilo editor -- version %s",
                        KILO_VERSION);
                if (result < 0)
                {
                    die("snprintf");
                }
                unsigned welcomelen = CAST(unsigned)result;
                if (welcomelen > editor.screencols)
                {
                    welcomelen = editor.screencols;
                }

                unsigned padding = (editor.screencols - welcomelen) / 2;
                if (padding)
                {
                    buffer_append(buffer, "~", 1);
                    while (--padding)
                    {
                        buffer_append(buffer, " ", 1);
                    }
                }

                buffer_append(buffer, welcome, CAST(unsigned)welcomelen);
            }
            else
            {
                buffer_append(buffer, "~", 1);
            }
        }
        else
        {
            Line *line = editor.lines + row;
            unsigned len = line->render.len;
            if (len < editor.coloff)
            {
                len = 0;
            }
            else
            {
                len -= editor.coloff;
                if (len > editor.screencols)
                {
                    len = editor.screencols;
                }
            }

            buffer_append(buffer, line->render.b + editor.coloff, len);
        }

        buffer_append(buffer, "\x1b[K", 3);
        buffer_append(buffer, "\r\n", 2);
    }
}


static void
editorDrawStatusBar(Buffer *buffer)
{
    buffer_append(buffer, "\x1b[7m", 4);
    char status[80];
    int result = snprintf(
        status, sizeof(status),
        "%.20s - %u lines%s",
        editor.filename ? editor.filename : "[No Name]",
        editor.numlines,
        editor.dirty ? " (modified)" : "");
    if (result < 0)
    {
        die("status bar");
    }

    unsigned len = CAST(unsigned)result;
    if (len > editor.screencols)
    {
        len = editor.screencols;
    }
    buffer_append(buffer, status, len);

    result = snprintf(status, sizeof(status), "%u/%u", editor.cy + 1, editor.numlines);
    if (result < 0)
    {
        die("status bar");
    }
    unsigned pos = len;
    len = CAST(unsigned)result;

    while (pos < editor.screencols)
    {
        if ((pos + len) == editor.screencols)
        {
            buffer_append(buffer, status, len);
            break;
        }
        else
        {
            ++pos;
            buffer_append(buffer, " ", 1);
        }
    }

    buffer_append(buffer, "\x1b[m\r\n", 5);
}


static void
editorDrawMessageBar(Buffer *buffer)
{
    buffer_append(buffer, "\x1b[K", 3);
    unsigned msglen = CAST(unsigned)strlen(editor.statusmsg);
    if (msglen > editor.screencols)
    {
        msglen = editor.screencols;
    }
    if (msglen && ((time(0) - editor.statusmsg_time) < 5))
    {
        buffer_append(buffer, editor.statusmsg, msglen);
    }
}


static void
editorRefreshScreen(void)
{
    editorScroll();

    Buffer buffer;
    buffer_init(&buffer);

    buffer_append(&buffer, "\x1b[?25l", 6);
    buffer_append(&buffer, "\x1b[H", 3);

    editorDrawRows(&buffer);
    editorDrawStatusBar(&buffer);
    editorDrawMessageBar(&buffer);

    char buf[32];
    snprintf(
        buf, sizeof(buf), "\x1b[%d;%dH",
        editor.cy - editor.rowoff + 1,
        editor.rx - editor.coloff + 1);
    buffer_append(&buffer, buf, CAST(unsigned)strlen(buf));

    buffer_append(&buffer, "\x1b[?25h", 6);

    write(STDOUT_FILENO, buffer.b, buffer.len);
    buffer_free(&buffer);
}


static void
editorSetStatusMessage(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(editor.statusmsg, sizeof(editor.statusmsg), fmt, args);
    va_end(args);
    editor.statusmsg_time = time(0);
}


//
// init
//

static void
initEditor(void)
{
    editor.cx = 0;
    editor.cy = 0;
    editor.rx = 0;
    editor.rowoff = 0;
    editor.coloff = 0;
    editor.numlines = 0;
    editor.dirty = 0;
    editor.lines = 0;
    editor.filename = 0;
    editor.statusmsg[0] = 0;
    editor.statusmsg_time = 0;
    if (getWindowSize(&editor.screenrows, &editor.screencols))
    {
        die("getWindowSize");
    }
    editor.screenrows -= 2;
}


int main(int argc, char **argv)
{
    initEditor();
    if (argc > 1)
    {
        editorOpen(argv[1]);
    }
    editorSetStatusMessage("Help: Ctrl-s = save | Ctrl-Q = quit | Ctrl-F = find");

    enableRawMode();
    for (;;)
    {
        editorRefreshScreen();
        editorProcessKeyPress();
    }

    return EXIT_SUCCESS;
}
